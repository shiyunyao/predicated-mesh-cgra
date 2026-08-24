// SPDX-License-Identifier: MIT
#include "cgra/Schedule/MaterializedScheduleVerifier.h"

#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace cgra::schedule {
namespace {

using cgra::register_allocation::StorageSegmentId;
using Event = MaterializedEvent;

struct PeriodicKey {
  MaterializedEventKind kind;
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  friend bool operator<(const PeriodicKey& lhs, const PeriodicKey& rhs) {
    return std::tie(lhs.kind, lhs.first, lhs.second) < std::tie(rhs.kind, rhs.first, rhs.second);
  }
};

struct BoundaryKey {
  cgra::target::TargetEdgeId edge = 0;
  std::uint32_t offset = 0;
  friend bool operator<(const BoundaryKey& lhs, const BoundaryKey& rhs) {
    return std::tie(lhs.edge, lhs.offset) < std::tie(rhs.edge, rhs.offset);
  }
};

struct LiveOutKey {
  cgra::ir::LiveOutId id = 0;
  friend bool operator<(const LiveOutKey& lhs, const LiveOutKey& rhs) { return lhs.id < rhs.id; }
};

void add(MaterializedScheduleVerificationReport& report, MaterializedScheduleVerificationCode code,
         std::string message, std::optional<cgra::target::TargetNodeId> node = std::nullopt,
         std::optional<cgra::target::TargetEdgeId> edge = std::nullopt,
         std::optional<StorageSegmentId> segment = std::nullopt) {
  report.add({code, std::move(message), node, edge, segment});
}

bool addSigned(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))
    return false;
  out = lhs + rhs;
  return true;
}

bool subtractSigned(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) {
  if (rhs == std::numeric_limits<std::int64_t>::min())
    return false;
  return addSigned(lhs, -rhs, out);
}

bool mulSigned(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) {
  if (lhs == 0 || rhs == 0) {
    out = 0;
    return true;
  }
  if (lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min())
    return false;
  if (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min())
    return false;
  if (lhs > 0 && rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() / rhs)
    return false;
  if (lhs < 0 && rhs < 0 && lhs < std::numeric_limits<std::int64_t>::max() / rhs)
    return false;
  if (lhs > 0 && rhs < 0 && rhs < std::numeric_limits<std::int64_t>::min() / lhs)
    return false;
  if (lhs < 0 && rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() / rhs)
    return false;
  out = lhs * rhs;
  return true;
}

bool toSigned(std::uint64_t value, std::int64_t& out) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  out = static_cast<std::int64_t>(value);
  return true;
}

bool expectedCycle(const StagedMapping& staged, cgra::target::TargetNodeId node,
                   std::int64_t iteration, std::uint32_t ii, std::uint64_t shift,
                   std::uint64_t& cycle) {
  std::int64_t base = 0;
  std::int64_t signedShift = 0;
  if (!toSigned(staged.logicalIssueTime(node), base) || !toSigned(shift, signedShift))
    return false;
  std::int64_t delta = 0;
  std::int64_t result = 0;
  if (!mulSigned(iteration, ii, delta) || !addSigned(base, delta, result) ||
      !addSigned(result, signedShift, result) || result < 0)
    return false;
  cycle = static_cast<std::uint64_t>(result);
  return true;
}

bool expectedCycleForEdge(const StagedMapping& staged, const cgra::target::TargetEdge& edge,
                          std::uint32_t elapsed, std::int64_t producerIteration, std::uint32_t ii,
                          std::uint64_t shift, std::uint64_t& cycle) {
  if (!expectedCycle(staged, edge.src, producerIteration, ii, shift, cycle))
    return false;
  std::int64_t value = 0;
  if (!addSigned(static_cast<std::int64_t>(cycle), elapsed, value) || value < 0)
    return false;
  cycle = static_cast<std::uint64_t>(value);
  return true;
}

void addRange(std::map<PeriodicKey, std::vector<std::pair<std::int64_t, std::int64_t>>>& ranges,
              PeriodicKey key, std::int64_t first, std::int64_t last) {
  ranges[key].emplace_back(first, last);
}

bool rangesCoverExactly(const std::vector<std::pair<std::int64_t, std::int64_t>>& input,
                        std::int64_t expectedFirst, std::int64_t expectedLast) {
  auto ranges = input;
  std::sort(ranges.begin(), ranges.end());
  if (ranges.empty() || ranges.front().first != expectedFirst)
    return false;
  std::int64_t end = ranges.front().second;
  for (std::size_t index = 1; index < ranges.size(); ++index) {
    if (ranges[index].first <= end)
      return false;
    if (end == std::numeric_limits<std::int64_t>::max() || ranges[index].first != end + 1)
      return false;
    end = ranges[index].second;
  }
  return end == expectedLast;
}

} // namespace

bool MaterializedScheduleVerificationReport::contains(
    MaterializedScheduleVerificationCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string MaterializedScheduleVerificationReport::format() const {
  std::ostringstream output;
  output << (ok() ? "valid" : "invalid") << " materialized schedule";
  for (const auto& diagnostic : diagnostics_) {
    output << '\n' << diagnostic.message;
    if (diagnostic.node)
      output << " node=" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    if (diagnostic.segment)
      output << " segment=" << *diagnostic.segment;
  }
  return output.str();
}

MaterializedScheduleVerificationReport MaterializedScheduleVerifier::verify(
    const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
    const cgra::register_allocation::RFAllocatedMapping& mapping,
    const ScheduleMaterializationRequest& request, const MaterializedSchedule& schedule) {
  MaterializedScheduleVerificationReport report;
  if (request.tripCount == 0) {
    add(report, MaterializedScheduleVerificationCode::MAT_INVALID_TRIP_COUNT,
        "trip count must be positive");
    return report;
  }
  if (request.tripCount > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
        "trip count does not fit signed event provenance");
    return report;
  }
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    add(report, MaterializedScheduleVerificationCode::MAT_INVALID_TARGET_DFG,
        targetReport.format());
    return report;
  }
  const auto rfReport =
      cgra::register_allocation::RFAllocationVerifier::verify(dfg, target, mapping);
  if (!rfReport.ok()) {
    add(report, MaterializedScheduleVerificationCode::MAT_INVALID_RF_MAPPING, rfReport.format());
    return report;
  }
  if (schedule.tripCount() != request.tripCount ||
      schedule.ii() != mapping.staged().modulo().ii()) {
    add(report, MaterializedScheduleVerificationCode::MAT_INVALID_TRIP_COUNT,
        "materialized schedule metadata disagrees with request or mapping");
    return report;
  }
  if ((schedule.kernel().repeatCount == 0 && !schedule.kernel().body.empty()) ||
      (schedule.kernel().repeatCount != 0 && schedule.kernel().body.size() != schedule.ii())) {
    add(report, MaterializedScheduleVerificationCode::MAT_KERNEL_SHAPE_INVALID,
        "kernel body must be empty when not repeated and contain exactly II cycles otherwise");
    return report;
  }
  if (schedule.totalLogicalCycles() == 0) {
    add(report, MaterializedScheduleVerificationCode::MAT_KERNEL_SHAPE_INVALID,
        "materialized schedule must contain at least one cycle");
    return report;
  }

  const auto& staged = mapping.staged();
  const auto ii = schedule.ii();
  std::int64_t expectedMinLogical = std::numeric_limits<std::int64_t>::max();
  std::int64_t expectedMaxLogical = std::numeric_limits<std::int64_t>::min();
  auto accountExpected = [&](std::int64_t first, std::uint64_t count) {
    if (count == 0 ||
        count - 1 > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return false;
    std::int64_t span = 0;
    std::int64_t last = 0;
    if (!mulSigned(static_cast<std::int64_t>(count - 1), ii, span) || !addSigned(first, span, last))
      return false;
    expectedMinLogical = std::min(expectedMinLogical, first);
    expectedMaxLogical = std::max(expectedMaxLogical, last);
    return true;
  };
  auto accountOneExpected = [&](std::int64_t time) {
    expectedMinLogical = std::min(expectedMinLogical, time);
    expectedMaxLogical = std::max(expectedMaxLogical, time);
  };
  bool expectedTimeValid = true;
  for (const auto& node : dfg.nodes()) {
    std::int64_t nodeTime = 0;
    if (!toSigned(staged.logicalIssueTime(node.id), nodeTime) ||
        !accountExpected(nodeTime, request.tripCount))
      expectedTimeValid = false;
  }
  for (const auto& edge : dfg.edges()) {
    if (edge.kind() == cgra::ir::Edge::Kind::Memory)
      continue;
    const auto& dependence = staged.modulo().dependence(edge.id);
    if (!dependence.transport) {
      expectedTimeValid = false;
      continue;
    }
    std::int64_t sourceTime = 0;
    std::int64_t distanceTime = 0;
    std::int64_t firstBase = 0;
    if (!toSigned(staged.logicalIssueTime(edge.src), sourceTime) ||
        !mulSigned(static_cast<std::int64_t>(edge.distance), ii, distanceTime) ||
        !subtractSigned(sourceTime, distanceTime, firstBase)) {
      expectedTimeValid = false;
      continue;
    }
    for (const auto& action : dependence.transport->actions) {
      const auto* link = std::get_if<cgra::mapping::LinkStep>(&action);
      if (!link)
        continue;
      std::int64_t first = 0;
      if (!addSigned(firstBase, static_cast<std::int64_t>(link->elapsedFromProducerIssue), first) ||
          !accountExpected(first, request.tripCount))
        expectedTimeValid = false;
    }
    const auto boundaryCount = std::min<std::uint64_t>(request.tripCount, edge.distance);
    for (std::uint32_t offset = 0; offset < boundaryCount; ++offset) {
      std::int64_t negativeIteration = 0;
      std::int64_t iterationTime = 0;
      std::int64_t boundaryTime = 0;
      if (!mulSigned(-1, static_cast<std::int64_t>(edge.distance - offset), negativeIteration) ||
          !mulSigned(negativeIteration, ii, iterationTime) ||
          !addSigned(sourceTime, iterationTime, boundaryTime) ||
          !addSigned(
              boundaryTime,
              static_cast<std::int64_t>(dfg.node(edge.src).producerOutputReadyOffset.value_or(0U)),
              boundaryTime))
        expectedTimeValid = false;
      else
        accountOneExpected(boundaryTime);
    }
  }
  for (const auto& segment : mapping.storageRequirements().segments()) {
    std::int64_t base = 0;
    std::int64_t distanceTime = 0;
    std::int64_t first = 0;
    if (!toSigned(segment.writeTime, base) ||
        !mulSigned(static_cast<std::int64_t>(dfg.edge(segment.edge).distance), ii, distanceTime) ||
        !subtractSigned(base, distanceTime, first) || !accountExpected(first, request.tripCount))
      expectedTimeValid = false;
    if (!toSigned(segment.readTime, base) || !subtractSigned(base, distanceTime, first) ||
        !accountExpected(first, request.tripCount))
      expectedTimeValid = false;
  }
  for (const auto& liveOut : dfg.liveOuts()) {
    std::int64_t sourceTime = 0;
    std::int64_t iterationTime = 0;
    std::int64_t liveOutTime = 0;
    const auto& node = dfg.node(liveOut.source);
    if (!node.resultLatency || !toSigned(staged.logicalIssueTime(liveOut.source), sourceTime) ||
        !mulSigned(static_cast<std::int64_t>(request.tripCount - 1), ii, iterationTime) ||
        !addSigned(sourceTime, iterationTime, liveOutTime) ||
        !addSigned(liveOutTime, static_cast<std::int64_t>(*node.resultLatency), liveOutTime))
      expectedTimeValid = false;
    else
      accountOneExpected(liveOutTime);
  }
  if (!expectedTimeValid || expectedMinLogical == std::numeric_limits<std::int64_t>::max() ||
      expectedMaxLogical == std::numeric_limits<std::int64_t>::min() ||
      expectedMinLogical == std::numeric_limits<std::int64_t>::min()) {
    add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
        "expected materialized event bounds overflow signed time");
    return report;
  }
  const auto expectedShiftSigned = expectedMinLogical < 0 ? -expectedMinLogical : 0;
  const auto expectedShift = static_cast<std::uint64_t>(expectedShiftSigned);
  if (expectedMaxLogical > std::numeric_limits<std::int64_t>::max() - expectedShiftSigned ||
      static_cast<std::uint64_t>(expectedMaxLogical + expectedShiftSigned) ==
          std::numeric_limits<std::uint64_t>::max()) {
    add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
        "expected materialized cycle bound overflows uint64");
    return report;
  }
  const auto expectedTotalCycles =
      static_cast<std::uint64_t>(expectedMaxLogical + expectedShiftSigned) + 1;
  if (schedule.timeOriginShift() != expectedShift ||
      schedule.totalLogicalCycles() != expectedTotalCycles) {
    add(report, MaterializedScheduleVerificationCode::MAT_KERNEL_SHAPE_INVALID,
        "schedule time origin or total cycle bound disagrees with semantic event streams");
    return report;
  }
  const auto kernelStart = static_cast<std::uint64_t>(schedule.prologue().cycles.size());
  if (schedule.kernel().repeatCount != 0 &&
      ii > std::numeric_limits<std::uint64_t>::max() / schedule.kernel().repeatCount) {
    add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
        "kernel cycle count overflows uint64");
    return report;
  }
  const auto kernelCycles = schedule.kernel().repeatCount * static_cast<std::uint64_t>(ii);
  if (kernelStart > schedule.totalLogicalCycles() ||
      kernelCycles > schedule.totalLogicalCycles() - kernelStart ||
      schedule.epilogue().cycles.size() !=
          schedule.totalLogicalCycles() - kernelStart - kernelCycles) {
    add(report, MaterializedScheduleVerificationCode::MAT_KERNEL_SHAPE_INVALID,
        "phase cycle counts do not cover the declared finite schedule");
    return report;
  }

  std::map<PeriodicKey, std::vector<std::pair<std::int64_t, std::int64_t>>> ranges;
  std::set<BoundaryKey> boundaryEvents;
  std::set<LiveOutKey> liveOutEvents;
  auto verifyPeriodic = [&](const Event& event, std::uint64_t cycle, bool recordRange) {
    if (event.kind == MaterializedEventKind::NodeIssue) {
      if (!event.node || !dfg.containsNode(*event.node) || event.logicalIteration < 0 ||
          static_cast<std::uint64_t>(event.logicalIteration) >= request.tripCount) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "node issue event has invalid node or iteration", event.node);
        return;
      }
      std::uint64_t expected = 0;
      if (!expectedCycle(staged, *event.node, event.logicalIteration, ii,
                         schedule.timeOriginShift(), expected) ||
          expected != cycle)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_TIME,
            "node issue time does not match its staged periodic stream", event.node);
      if (recordRange)
        addRange(ranges, {event.kind, *event.node, 0}, event.logicalIteration,
                 event.logicalIteration);
      return;
    }
    if (event.kind != MaterializedEventKind::LinkLaunch &&
        event.kind != MaterializedEventKind::RFRead &&
        event.kind != MaterializedEventKind::RFWrite) {
      add(report, MaterializedScheduleVerificationCode::MAT_UNKNOWN_EVENT,
          "unexpected event in periodic stream");
      return;
    }
    if (!event.edge && !event.segment) {
      add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
          "periodic event lacks edge or storage provenance");
      return;
    }
    if (event.kind == MaterializedEventKind::LinkLaunch) {
      if (!event.edge || !dfg.containsEdge(*event.edge) || !event.transportActionIndex) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "link launch references an invalid edge or action", std::nullopt, event.edge);
        return;
      }
      const auto& edge = dfg.edge(*event.edge);
      if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
        add(report, MaterializedScheduleVerificationCode::MAT_MEMORY_TRANSPORT_EVENT,
            "memory edge cannot produce a materialized link launch", std::nullopt, event.edge);
        return;
      }
      const auto& transport = mapping.staged().modulo().dependence(edge.id).transport;
      if (!transport || *event.transportActionIndex >= transport->actions.size() ||
          !std::holds_alternative<cgra::mapping::LinkStep>(
              transport->actions[*event.transportActionIndex])) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "link launch action is not present in the mapped transport", std::nullopt, event.edge);
        return;
      }
      const auto& link =
          std::get<cgra::mapping::LinkStep>(transport->actions[*event.transportActionIndex]);
      if (!event.domain || !event.tile || !event.direction || *event.domain != link.domain ||
          *event.tile != link.source || *event.direction != link.direction)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "link launch geometry or domain disagrees with the mapped action", std::nullopt,
            event.edge);
      std::uint64_t expected = 0;
      if (!expectedCycleForEdge(staged, edge, link.elapsedFromProducerIssue, event.logicalIteration,
                                ii, schedule.timeOriginShift(), expected) ||
          expected != cycle)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_TIME,
            "link launch time does not match mapped route timing", std::nullopt, event.edge);
      const auto first = -static_cast<std::int64_t>(edge.distance);
      const auto last = static_cast<std::int64_t>(request.tripCount) - 1 - edge.distance;
      if (event.logicalIteration < first || event.logicalIteration > last)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "link launch producer iteration is outside the finite loop", std::nullopt, event.edge);
      if (recordRange)
        addRange(ranges, {event.kind, edge.id, *event.transportActionIndex}, event.logicalIteration,
                 event.logicalIteration);
      return;
    }
    if (!event.segment) {
      add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
          "RF event lacks a storage segment");
      return;
    }
    try {
      const auto& segment = mapping.storageRequirements().segment(*event.segment);
      if (!event.edge || *event.edge != segment.edge)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "RF event edge disagrees with its storage segment", std::nullopt, event.edge,
            segment.id);
      const auto expectedDomain = segment.domain == cgra::RegisterBankDomain::Predicate
                                      ? cgra::mapping::NetworkDomain::Predicate
                                      : cgra::mapping::NetworkDomain::Data;
      if (!event.domain || !event.tile || *event.domain != expectedDomain ||
          *event.tile != segment.tile)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "RF event tile or domain disagrees with its storage segment", std::nullopt,
            segment.edge, segment.id);
      if (!event.physicalRegister || *event.physicalRegister != mapping.registerFor(segment.id))
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "RF event physical register disagrees with allocation", std::nullopt, segment.edge,
            segment.id);
      const auto& edge = dfg.edge(segment.edge);
      const auto first = -static_cast<std::int64_t>(edge.distance);
      const auto last = static_cast<std::int64_t>(request.tripCount) - 1 - edge.distance;
      if (event.logicalIteration < first || event.logicalIteration > last)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "RF event producer iteration is outside the finite loop", std::nullopt, segment.edge,
            segment.id);
      const auto base =
          event.kind == MaterializedEventKind::RFWrite ? segment.writeTime : segment.readTime;
      std::int64_t signedBase = 0;
      std::int64_t signedShift = 0;
      if (!toSigned(base, signedBase) || !toSigned(schedule.timeOriginShift(), signedShift)) {
        add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
            "RF event base time does not fit signed time", std::nullopt, segment.edge, segment.id);
        return;
      }
      std::int64_t producerDelta = 0;
      std::int64_t expectedSigned = 0;
      if (!mulSigned(event.logicalIteration, ii, producerDelta) ||
          !addSigned(signedBase, producerDelta, expectedSigned) ||
          !addSigned(expectedSigned, signedShift, expectedSigned) || expectedSigned < 0 ||
          static_cast<std::uint64_t>(expectedSigned) != cycle)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_TIME,
            "RF event time does not match storage lifetime stream", std::nullopt, segment.edge,
            segment.id);
      if (recordRange)
        addRange(ranges, {event.kind, segment.id, 0}, event.logicalIteration,
                 event.logicalIteration);
    } catch (const std::out_of_range&) {
      add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
          "RF event references an unknown storage segment", std::nullopt, event.edge,
          event.segment);
    }
  };
  auto verifyOneShot = [&](const Event& event, std::uint64_t cycle, bool inKernel) {
    if (inKernel) {
      add(report, MaterializedScheduleVerificationCode::MAT_KERNEL_SHAPE_INVALID,
          "one-shot event appears in repeating kernel", event.node, event.edge, event.segment);
      return;
    }
    if (event.kind == MaterializedEventKind::BoundaryValueInject) {
      if (!event.edge || !event.consumerIterationOffset || !event.boundaryValue ||
          !dfg.containsEdge(*event.edge)) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "boundary injection lacks valid provenance", std::nullopt, event.edge);
        return;
      }
      const auto& edge = dfg.edge(*event.edge);
      if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
        add(report, MaterializedScheduleVerificationCode::MAT_MEMORY_TRANSPORT_EVENT,
            "memory edge cannot produce a recurrence boundary injection", std::nullopt, event.edge);
        return;
      }
      const auto count = std::min<std::uint64_t>(request.tripCount, edge.distance);
      if (*event.consumerIterationOffset >= count ||
          event.logicalIteration != static_cast<std::int64_t>(*event.consumerIterationOffset) -
                                        static_cast<std::int64_t>(edge.distance)) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "boundary injection offset is invalid", std::nullopt, event.edge);
        return;
      }
      const auto boundary = edge.kind() == cgra::ir::Edge::Kind::Predicate
                                ? std::get<cgra::ir::PredicateEdgeInfo>(edge.info).boundary
                                : std::get<cgra::ir::DataEdgeInfo>(edge.info).boundary;
      if (!boundary) {
        add(report, MaterializedScheduleVerificationCode::MAT_MISSING_EVENT,
            "boundary injection has no corresponding IR boundary", std::nullopt, event.edge);
        return;
      }
      const auto found =
          std::find_if(boundary->values.begin(), boundary->values.end(), [&](const auto& value) {
            return value.iterationOffset == *event.consumerIterationOffset;
          });
      if (found == boundary->values.end() || found->value != *event.boundaryValue)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "boundary injection value disagrees with IR boundary", std::nullopt, event.edge);
      const auto expectedDomain = edge.kind() == cgra::ir::Edge::Kind::Predicate
                                      ? cgra::mapping::NetworkDomain::Predicate
                                      : cgra::mapping::NetworkDomain::Data;
      const auto expectedTile = staged.modulo().placement(edge.src).tile;
      if (!event.domain || !event.tile || *event.domain != expectedDomain ||
          *event.tile != expectedTile)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "boundary injection source tile or domain is invalid", std::nullopt, event.edge);
      std::uint64_t expected = 0;
      const auto& source = dfg.node(edge.src);
      if (!expectedCycle(staged, edge.src, event.logicalIteration, ii, schedule.timeOriginShift(),
                         expected) ||
          expected > std::numeric_limits<std::uint64_t>::max() -
                         source.producerOutputReadyOffset.value_or(0U) ||
          expected + source.producerOutputReadyOffset.value_or(0U) != cycle)
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_TIME,
            "boundary injection time disagrees with producer readiness", std::nullopt, event.edge);
      if (!boundaryEvents.insert({edge.id, *event.consumerIterationOffset}).second)
        add(report, MaterializedScheduleVerificationCode::MAT_DUPLICATE_EVENT,
            "recurrence boundary injection is duplicated", std::nullopt, event.edge);
      return;
    }
    if (event.kind == MaterializedEventKind::LiveOutBoundaryUse) {
      const auto liveOutIt =
          event.liveOut
              ? std::find_if(dfg.liveOuts().begin(), dfg.liveOuts().end(),
                             [&](const auto& liveOut) { return liveOut.id == *event.liveOut; })
              : dfg.liveOuts().end();
      if (!event.liveOut || liveOutIt == dfg.liveOuts().end() || !event.node) {
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_PROVENANCE,
            "live-out event lacks valid provenance", event.node);
        return;
      }
      const auto& liveOut = *liveOutIt;
      const auto& node = dfg.node(liveOut.source);
      std::uint64_t expected = 0;
      if (!node.resultLatency ||
          !expectedCycle(staged, liveOut.source, static_cast<std::int64_t>(request.tripCount - 1),
                         ii, schedule.timeOriginShift(), expected) ||
          expected > std::numeric_limits<std::uint64_t>::max() - *node.resultLatency ||
          expected + *node.resultLatency != cycle || event.node != liveOut.source ||
          event.logicalIteration != static_cast<std::int64_t>(request.tripCount - 1))
        add(report, MaterializedScheduleVerificationCode::MAT_WRONG_EVENT_TIME,
            "live-out event time or source is invalid", liveOut.source);
      if (!liveOutEvents.insert(LiveOutKey{liveOut.id}).second)
        add(report, MaterializedScheduleVerificationCode::MAT_DUPLICATE_EVENT,
            "live-out boundary event is duplicated", liveOut.source);
      return;
    }
    add(report, MaterializedScheduleVerificationCode::MAT_UNKNOWN_EVENT,
        "unknown one-shot event kind");
  };

  auto scanPhase = [&](const SchedulePhase& phase, std::uint64_t base, bool kernel) {
    for (std::size_t offset = 0; offset < phase.cycles.size(); ++offset) {
      if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
        add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
            "schedule cycle index overflows uint64");
        return;
      }
      for (const auto& event : phase.cycles[offset].events) {
        const auto cycle = base + offset;
        if (event.kind == MaterializedEventKind::BoundaryValueInject ||
            event.kind == MaterializedEventKind::LiveOutBoundaryUse)
          verifyOneShot(event, cycle, kernel);
        else
          verifyPeriodic(event, cycle, true);
      }
    }
  };
  scanPhase(schedule.prologue(), 0, false);
  for (std::size_t offset = 0; offset < schedule.kernel().body.size(); ++offset) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - kernelStart) {
      add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
          "kernel cycle index overflows uint64");
      break;
    }
    const auto cycle = kernelStart + offset;
    for (const auto& event : schedule.kernel().body[offset].events) {
      if (event.kind == MaterializedEventKind::BoundaryValueInject ||
          event.kind == MaterializedEventKind::LiveOutBoundaryUse)
        verifyOneShot(event, cycle, true);
      else {
        verifyPeriodic(event, cycle, false);
        const auto key = event.kind == MaterializedEventKind::NodeIssue
                             ? PeriodicKey{event.kind, event.node.value_or(0), 0}
                         : event.kind == MaterializedEventKind::LinkLaunch
                             ? PeriodicKey{event.kind, event.edge.value_or(0),
                                           event.transportActionIndex.value_or(0)}
                             : PeriodicKey{event.kind, event.segment.value_or(0), 0};
        if (schedule.kernel().repeatCount > 0) {
          std::int64_t repeatDelta = 0;
          std::int64_t rangeEnd = 0;
          if (!toSigned(schedule.kernel().repeatCount - 1, repeatDelta) ||
              !addSigned(event.logicalIteration, repeatDelta, rangeEnd))
            add(report, MaterializedScheduleVerificationCode::MAT_OUTPUT_OVERFLOW,
                "kernel event iteration range overflows signed time");
          else
            addRange(ranges, key, event.logicalIteration, rangeEnd);
        }
      }
    }
  }
  scanPhase(schedule.epilogue(), kernelStart + kernelCycles, false);

  std::map<PeriodicKey, std::pair<std::int64_t, std::int64_t>> expected;
  for (const auto& node : dfg.nodes())
    expected[{MaterializedEventKind::NodeIssue, node.id, 0}] = {
        0, static_cast<std::int64_t>(request.tripCount) - 1};
  for (const auto& edge : dfg.edges()) {
    if (edge.kind() == cgra::ir::Edge::Kind::Memory)
      continue;
    const auto& dep = staged.modulo().dependence(edge.id);
    if (!dep.transport)
      continue;
    for (std::size_t action = 0; action < dep.transport->actions.size(); ++action)
      if (std::holds_alternative<cgra::mapping::LinkStep>(dep.transport->actions[action]))
        expected[{MaterializedEventKind::LinkLaunch, edge.id, action}] = {
            -static_cast<std::int64_t>(edge.distance),
            static_cast<std::int64_t>(request.tripCount) - 1 - edge.distance};
  }
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto first = -static_cast<std::int64_t>(dfg.edge(segment.edge).distance);
    const auto last = static_cast<std::int64_t>(request.tripCount) - 1 -
                      static_cast<std::int64_t>(dfg.edge(segment.edge).distance);
    expected[{MaterializedEventKind::RFWrite, segment.id, 0}] = {first, last};
    expected[{MaterializedEventKind::RFRead, segment.id, 0}] = {first, last};
  }
  for (const auto& [key, range] : expected) {
    const auto found = ranges.find(key);
    if (found == ranges.end() || !rangesCoverExactly(found->second, range.first, range.second))
      add(report, MaterializedScheduleVerificationCode::MAT_MISSING_EVENT,
          "periodic event stream is missing or duplicated", std::nullopt,
          key.kind == MaterializedEventKind::LinkLaunch
              ? std::optional{static_cast<std::uint32_t>(key.first)}
              : std::nullopt,
          key.kind == MaterializedEventKind::RFRead || key.kind == MaterializedEventKind::RFWrite
              ? std::optional{static_cast<StorageSegmentId>(key.first)}
              : std::nullopt);
  }
  for (const auto& [key, rangesForKey] : ranges)
    if (!expected.contains(key))
      add(report, MaterializedScheduleVerificationCode::MAT_DUPLICATE_EVENT,
          "schedule contains an unexpected periodic event stream");

  for (const auto& edge : dfg.edges()) {
    if (edge.kind() == cgra::ir::Edge::Kind::Memory)
      continue;
    const auto count = std::min<std::uint64_t>(request.tripCount, edge.distance);
    for (std::uint32_t offset = 0; offset < count; ++offset)
      if (!boundaryEvents.contains({edge.id, offset}))
        add(report, MaterializedScheduleVerificationCode::MAT_MISSING_EVENT,
            "recurrence boundary injection is missing", std::nullopt, edge.id);
  }
  for (const auto& liveOut : dfg.liveOuts())
    if (!liveOutEvents.contains(LiveOutKey{liveOut.id}))
      add(report, MaterializedScheduleVerificationCode::MAT_MISSING_EVENT,
          "live-out boundary event is missing", liveOut.source);
  return report;
}

} // namespace cgra::schedule
