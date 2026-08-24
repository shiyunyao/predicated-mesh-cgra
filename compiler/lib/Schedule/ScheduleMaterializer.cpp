// SPDX-License-Identifier: MIT
#include "cgra/Schedule/ScheduleMaterializer.h"

#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/Schedule/MaterializedScheduleVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace cgra::schedule {
namespace {

using cgra::mapping::NetworkDomain;
using cgra::register_allocation::StorageSegmentId;
using Event = MaterializedEvent;

struct PeriodicStream {
  Event event;
  std::int64_t firstLogicalTime = 0;
  std::uint64_t count = 0;
  std::uint32_t period = 0;
};

struct OneShot {
  Event event;
  std::int64_t logicalTime = 0;
};

void add(ScheduleMaterializationResult& result, ScheduleMaterializationDiagnosticCode code,
         std::string message, std::optional<cgra::target::TargetNodeId> node = std::nullopt,
         std::optional<cgra::target::TargetEdgeId> edge = std::nullopt,
         std::optional<StorageSegmentId> segment = std::nullopt,
         std::optional<std::int64_t> iteration = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), node, edge, segment, iteration});
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

bool addUnsigned(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    return false;
  out = lhs + rhs;
  return true;
}

bool mulUnsigned(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
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

std::int64_t signedTime(const StagedMapping& mapping, cgra::target::TargetNodeId node) {
  std::int64_t value = 0;
  if (!toSigned(mapping.logicalIssueTime(node), value))
    throw std::overflow_error("logical issue time does not fit signed 64-bit time");
  return value;
}

NetworkDomain domainFor(cgra::ir::Edge::Kind kind) {
  return kind == cgra::ir::Edge::Kind::Predicate ? NetworkDomain::Predicate : NetworkDomain::Data;
}

std::optional<cgra::ir::ExternalOperandBinding> boundaryValue(const cgra::target::TargetEdge& edge,
                                                              std::uint32_t offset) {
  if (edge.kind() == cgra::ir::Edge::Kind::Memory)
    return std::nullopt;
  const auto& info = edge.kind() == cgra::ir::Edge::Kind::Predicate
                         ? std::get<cgra::ir::PredicateEdgeInfo>(edge.info).boundary
                         : std::get<cgra::ir::DataEdgeInfo>(edge.info).boundary;
  if (!info)
    return std::nullopt;
  for (const auto& value : info->values)
    if (value.iterationOffset == offset)
      return value.value;
  return std::nullopt;
}

int eventOrder(MaterializedEventKind kind) {
  switch (kind) {
  case MaterializedEventKind::BoundaryValueInject:
    return 0;
  case MaterializedEventKind::RFRead:
    return 1;
  case MaterializedEventKind::NodeIssue:
    return 2;
  case MaterializedEventKind::LinkLaunch:
    return 3;
  case MaterializedEventKind::RFWrite:
    return 4;
  case MaterializedEventKind::LiveOutBoundaryUse:
    return 5;
  }
  return 6;
}

bool eventLess(const Event& lhs, const Event& rhs) {
  if (eventOrder(lhs.kind) != eventOrder(rhs.kind))
    return eventOrder(lhs.kind) < eventOrder(rhs.kind);
  const auto lhsEdge = lhs.edge.value_or(std::numeric_limits<std::uint32_t>::max());
  const auto rhsEdge = rhs.edge.value_or(std::numeric_limits<std::uint32_t>::max());
  if (lhsEdge != rhsEdge)
    return lhsEdge < rhsEdge;
  const auto lhsNode = lhs.node.value_or(std::numeric_limits<std::uint32_t>::max());
  const auto rhsNode = rhs.node.value_or(std::numeric_limits<std::uint32_t>::max());
  if (lhsNode != rhsNode)
    return lhsNode < rhsNode;
  const auto lhsSegment = lhs.segment.value_or(std::numeric_limits<StorageSegmentId>::max());
  const auto rhsSegment = rhs.segment.value_or(std::numeric_limits<StorageSegmentId>::max());
  if (lhsSegment != rhsSegment)
    return lhsSegment < rhsSegment;
  const auto lhsAction =
      lhs.transportActionIndex.value_or(std::numeric_limits<std::uint32_t>::max());
  const auto rhsAction =
      rhs.transportActionIndex.value_or(std::numeric_limits<std::uint32_t>::max());
  if (lhsAction != rhsAction)
    return lhsAction < rhsAction;
  return lhs.logicalIteration < rhs.logicalIteration;
}

void sortBundles(SchedulePhase& phase) {
  for (auto& cycle : phase.cycles)
    std::stable_sort(cycle.events.begin(), cycle.events.end(), eventLess);
}

void sortKernel(RepeatingKernel& kernel) {
  for (auto& cycle : kernel.body)
    std::stable_sort(cycle.events.begin(), cycle.events.end(), eventLess);
}

} // namespace

ScheduleMaterializationResult
ScheduleMaterializer::materialize(const cgra::target::TargetDFG& dfg,
                                  const cgra::TargetModel& target,
                                  const cgra::register_allocation::RFAllocatedMapping& mapping,
                                  const ScheduleMaterializationRequest& request) {
  ScheduleMaterializationResult result;
  result.stats.tripCount = request.tripCount;
  if (request.tripCount == 0) {
    result.status = ScheduleMaterializationStatus::InvalidTripCount;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INVALID_TRIP_COUNT,
        "T011 requires a positive concrete trip count");
    return result;
  }
  if (request.tripCount > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    result.status = ScheduleMaterializationStatus::ArithmeticOverflow;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_TIME_ARITHMETIC_OVERFLOW,
        "trip count does not fit signed iteration provenance");
    return result;
  }
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    result.status = ScheduleMaterializationStatus::InvalidTargetDFG;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INVALID_TARGET_DFG,
        targetReport.format());
    return result;
  }
  const auto rfReport =
      cgra::register_allocation::RFAllocationVerifier::verify(dfg, target, mapping);
  if (!rfReport.ok()) {
    result.status = ScheduleMaterializationStatus::InvalidRFAllocatedMapping;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INVALID_RF_MAPPING, rfReport.format());
    return result;
  }
  const auto& staged = mapping.staged();
  const auto ii = staged.modulo().ii();
  if (ii == 0) {
    result.status = ScheduleMaterializationStatus::InvalidRFAllocatedMapping;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INVALID_RF_MAPPING,
        "RF allocated mapping has zero initiation interval");
    return result;
  }

  try {
    std::vector<PeriodicStream> streams;
    std::vector<OneShot> oneShots;
    std::int64_t minLogical = std::numeric_limits<std::int64_t>::max();
    std::int64_t maxLogical = std::numeric_limits<std::int64_t>::min();
    auto account = [&](std::int64_t first, std::uint64_t count, std::int64_t period) {
      if (count == 0 || period <= 0)
        throw std::invalid_argument("invalid periodic event stream");
      std::int64_t tail = 0;
      std::int64_t span = 0;
      if (!mulSigned(static_cast<std::int64_t>(count - 1), period, span) ||
          !addSigned(first, span, tail))
        throw std::overflow_error("periodic event stream exceeds signed 64-bit time");
      minLogical = std::min(minLogical, first);
      maxLogical = std::max(maxLogical, tail);
    };
    auto addOneShot = [&](OneShot shot) {
      minLogical = std::min(minLogical, shot.logicalTime);
      maxLogical = std::max(maxLogical, shot.logicalTime);
      oneShots.push_back(std::move(shot));
    };

    for (const auto& node : dfg.nodes()) {
      Event event;
      event.kind = MaterializedEventKind::NodeIssue;
      event.node = node.id;
      event.logicalIteration = 0;
      const auto first = signedTime(staged, node.id);
      streams.push_back({event, first, request.tripCount, ii});
      account(first, request.tripCount, ii);
    }

    for (const auto& edge : dfg.edges()) {
      if (edge.kind() == cgra::ir::Edge::Kind::Memory)
        continue;
      const auto& dependence = staged.modulo().dependence(edge.id);
      if (!dependence.transport)
        throw std::invalid_argument("value edge has no transport plan");
      const auto sourceTime = signedTime(staged, edge.src);
      std::int64_t distanceTime = 0;
      if (!mulSigned(static_cast<std::int64_t>(edge.distance), ii, distanceTime))
        throw std::overflow_error("edge distance exceeds signed 64-bit time");
      std::int64_t firstBase = 0;
      if (!subtractSigned(sourceTime, distanceTime, firstBase))
        throw std::overflow_error("producer pre-roll time overflows signed 64-bit time");
      const auto domain = domainFor(edge.kind());
      for (std::size_t actionIndex = 0; actionIndex < dependence.transport->actions.size();
           ++actionIndex) {
        const auto* link =
            std::get_if<cgra::mapping::LinkStep>(&dependence.transport->actions[actionIndex]);
        if (!link)
          continue;
        Event event;
        event.kind = MaterializedEventKind::LinkLaunch;
        event.edge = edge.id;
        event.transportActionIndex = static_cast<std::uint32_t>(actionIndex);
        event.logicalIteration = -static_cast<std::int64_t>(edge.distance);
        event.domain = link->domain;
        event.tile = link->source;
        event.direction = link->direction;
        std::int64_t first = 0;
        if (!addSigned(firstBase, static_cast<std::int64_t>(link->elapsedFromProducerIssue), first))
          throw std::overflow_error("link launch time overflows signed 64-bit time");
        streams.push_back({event, first, request.tripCount, ii});
        account(first, request.tripCount, ii);
      }

      const auto boundaryCount = std::min<std::uint64_t>(request.tripCount, edge.distance);
      for (std::uint32_t offset = 0; offset < boundaryCount; ++offset) {
        const auto value = boundaryValue(edge, offset);
        if (!value) {
          result.status = ScheduleMaterializationStatus::MissingRecurrenceBoundaryValue;
          add(result, ScheduleMaterializationDiagnosticCode::MAT_BOUNDARY_VALUE_MISSING,
              "loop-carried edge is missing a boundary value", std::nullopt, edge.id, std::nullopt,
              static_cast<std::int64_t>(offset));
          return result;
        }
        const auto source = dfg.node(edge.src);
        const auto producer = signedTime(staged, edge.src);
        std::int64_t negativeIteration = 0;
        if (!mulSigned(-1, static_cast<std::int64_t>(edge.distance - offset), negativeIteration))
          throw std::overflow_error("boundary iteration overflows signed 64-bit time");
        std::int64_t iterationTime = 0;
        if (!mulSigned(negativeIteration, ii, iterationTime))
          throw std::overflow_error("boundary producer time overflows signed 64-bit time");
        std::int64_t readyOffset = 0;
        if (!toSigned(source.producerOutputReadyOffset.value_or(0U), readyOffset))
          throw std::overflow_error("producer readiness offset overflows signed 64-bit time");
        std::int64_t ready = 0;
        if (!addSigned(producer, iterationTime, ready) || !addSigned(ready, readyOffset, ready))
          throw std::overflow_error("boundary availability time overflows signed 64-bit time");
        Event event;
        event.kind = MaterializedEventKind::BoundaryValueInject;
        event.edge = edge.id;
        event.domain = domain;
        event.tile = staged.modulo().placement(edge.src).tile;
        event.logicalIteration = negativeIteration;
        event.consumerIterationOffset = offset;
        event.boundaryValue = *value;
        addOneShot({event, ready});
      }
    }

    for (const auto& segment : mapping.storageRequirements().segments()) {
      const auto& edge = dfg.edge(segment.edge);
      if (edge.kind() == cgra::ir::Edge::Kind::Memory)
        throw std::invalid_argument("storage segment is attached to a memory edge");
      const auto distance = static_cast<std::int64_t>(edge.distance);
      std::int64_t distanceTime = 0;
      if (!mulSigned(distance, ii, distanceTime))
        throw std::overflow_error("storage segment distance overflows signed 64-bit time");
      for (const bool write : {true, false}) {
        const auto base = write ? segment.writeTime : segment.readTime;
        std::int64_t baseTime = 0;
        if (!toSigned(base, baseTime) || !subtractSigned(baseTime, distanceTime, baseTime))
          throw std::overflow_error("RF event time overflows signed 64-bit time");
        Event event;
        event.kind = write ? MaterializedEventKind::RFWrite : MaterializedEventKind::RFRead;
        event.edge = segment.edge;
        event.segment = segment.id;
        event.logicalIteration = -distance;
        event.domain = segment.domain == cgra::RegisterBankDomain::Predicate
                           ? NetworkDomain::Predicate
                           : NetworkDomain::Data;
        event.tile = segment.tile;
        event.physicalRegister = mapping.registerFor(segment.id);
        streams.push_back({event, baseTime, request.tripCount, ii});
        account(baseTime, request.tripCount, ii);
      }
    }

    for (const auto& liveOut : dfg.liveOuts()) {
      const auto& node = dfg.node(liveOut.source);
      if (!node.resultLatency)
        throw std::invalid_argument("live-out source has no result latency");
      std::int64_t sourceTime = signedTime(staged, liveOut.source);
      std::int64_t iterationTime = 0;
      if (!mulSigned(static_cast<std::int64_t>(request.tripCount - 1), ii, iterationTime))
        throw std::overflow_error("live-out iteration time overflows signed 64-bit time");
      std::int64_t time = 0;
      if (!addSigned(sourceTime, iterationTime, time) ||
          !addSigned(time, static_cast<std::int64_t>(*node.resultLatency), time))
        throw std::overflow_error("live-out time overflows signed 64-bit time");
      Event event;
      event.kind = MaterializedEventKind::LiveOutBoundaryUse;
      event.liveOut = liveOut.id;
      event.node = liveOut.source;
      event.logicalIteration = static_cast<std::int64_t>(request.tripCount - 1);
      addOneShot({event, time});
    }

    result.stats.periodicStreams = streams.size();
    result.stats.oneShotEvents = oneShots.size();
    result.stats.boundarySeedEvents = static_cast<std::uint64_t>(
        std::count_if(oneShots.begin(), oneShots.end(), [](const auto& shot) {
          return shot.event.kind == MaterializedEventKind::BoundaryValueInject;
        }));
    result.stats.liveOutEvents = static_cast<std::uint64_t>(
        std::count_if(oneShots.begin(), oneShots.end(), [](const auto& shot) {
          return shot.event.kind == MaterializedEventKind::LiveOutBoundaryUse;
        }));
    if (oneShots.size() > request.budget.maxExplicitBoundaryEvents) {
      result.status = ScheduleMaterializationStatus::MaterializationBudgetExceeded;
      add(result, ScheduleMaterializationDiagnosticCode::MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED,
          "explicit boundary event budget exceeded");
      return result;
    }

    if (minLogical == std::numeric_limits<std::int64_t>::max() ||
        maxLogical == std::numeric_limits<std::int64_t>::min())
      throw std::invalid_argument("target DFG has no materializable events");
    if (minLogical == std::numeric_limits<std::int64_t>::min())
      throw std::overflow_error("time-origin shift overflows signed 64-bit time");
    std::int64_t shiftSigned = minLogical < 0 ? -minLogical : 0;
    std::uint64_t shift = static_cast<std::uint64_t>(shiftSigned);
    result.stats.timeOriginShift = shift;
    auto shifted = [&](std::int64_t logical) -> std::uint64_t {
      std::int64_t value = 0;
      if (!addSigned(logical, shiftSigned, value) || value < 0)
        throw std::overflow_error("time-origin shift overflows schedule cycle");
      return static_cast<std::uint64_t>(value);
    };

    std::uint64_t maxCycle = shifted(maxLogical);
    if (maxCycle == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("schedule cycle bound overflows uint64");
    ++maxCycle;
    result.stats.totalLogicalCycles = maxCycle;
    for (auto& stream : streams) {
      const auto shiftedTime = shifted(stream.firstLogicalTime);
      if (!toSigned(shiftedTime, stream.firstLogicalTime))
        throw std::overflow_error("shifted periodic event time exceeds signed range");
    }
    for (auto& shot : oneShots) {
      const auto shiftedTime = shifted(shot.logicalTime);
      if (!toSigned(shiftedTime, shot.logicalTime))
        throw std::overflow_error("shifted one-shot event time exceeds signed range");
    }

    std::uint64_t begin = 0;
    std::uint64_t end = std::numeric_limits<std::uint64_t>::max();
    if (!streams.empty()) {
      for (const auto& stream : streams) {
        const auto firstCycle = static_cast<std::uint64_t>(stream.firstLogicalTime);
        const auto firstPeriod = firstCycle / ii;
        std::uint64_t lastPeriod = 0;
        if (!addUnsigned(firstPeriod, stream.count - 1, lastPeriod))
          throw std::overflow_error("periodic stream period range overflows uint64");
        begin = std::max(begin, firstPeriod);
        end = std::min(end, lastPeriod);
      }
    } else {
      end = 0;
    }
    auto hasOneShotInPeriod = [&](std::uint64_t period) {
      return std::any_of(oneShots.begin(), oneShots.end(), [&](const auto& shot) {
        return static_cast<std::uint64_t>(shot.logicalTime) / ii == period;
      });
    };
    while (begin <= end && hasOneShotInPeriod(begin)) {
      if (begin == std::numeric_limits<std::uint64_t>::max()) {
        begin = 0;
        end = 0;
        break;
      }
      ++begin;
    }
    while (begin <= end && hasOneShotInPeriod(end)) {
      if (end == 0) {
        begin = 1;
        end = 0;
        break;
      }
      --end;
    }
    if (begin <= end && std::any_of(oneShots.begin(), oneShots.end(), [&](const auto& shot) {
          const auto period = static_cast<std::uint64_t>(shot.logicalTime) / ii;
          return period >= begin && period <= end;
        })) {
      result.status = ScheduleMaterializationStatus::PhaseFactorizationError;
      add(result, ScheduleMaterializationDiagnosticCode::MAT_PHASE_FACTORIZATION_FAILED,
          "a one-shot event falls inside the repeating kernel range");
      return result;
    }

    const bool hasKernel = !streams.empty() && begin <= end;
    std::uint64_t kernelRepeatCount = 0;
    if (hasKernel && !addUnsigned(end - begin, 1, kernelRepeatCount))
      throw std::overflow_error("kernel repeat count overflows uint64");
    std::uint64_t kernelStartCycle = 0;
    std::uint64_t kernelEndCycle = 0;
    std::uint64_t endPlusOne = 0;
    if (hasKernel &&
        (!mulUnsigned(begin, ii, kernelStartCycle) || !addUnsigned(end, 1, endPlusOne) ||
         !mulUnsigned(endPlusOne, ii, kernelEndCycle)))
      throw std::overflow_error("kernel cycle range overflows uint64");
    std::uint64_t prologueCycles = hasKernel ? kernelStartCycle : maxCycle;
    std::uint64_t epilogueCycles = hasKernel ? maxCycle - kernelEndCycle : 0;
    if (prologueCycles > request.budget.maxExplicitBoundaryCycles ||
        epilogueCycles > request.budget.maxExplicitBoundaryCycles) {
      result.status = ScheduleMaterializationStatus::MaterializationBudgetExceeded;
      add(result, ScheduleMaterializationDiagnosticCode::MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED,
          "explicit prologue or epilogue cycle budget exceeded");
      return result;
    }

    SchedulePhase prologue{std::vector<CycleBundle>(prologueCycles)};
    SchedulePhase epilogue{std::vector<CycleBundle>(epilogueCycles)};
    RepeatingKernel kernel{std::vector<CycleBundle>(hasKernel ? ii : 0), kernelRepeatCount};
    std::uint64_t explicitEvents = 0;
    std::uint64_t plannedExplicitEvents = oneShots.size();
    auto appendExplicit = [&](const Event& event, std::uint64_t cycle) {
      if (cycle < (hasKernel ? kernelStartCycle : maxCycle))
        prologue.cycles[cycle].events.push_back(event);
      else {
        const auto index = cycle - kernelEndCycle;
        if (index >= epilogue.cycles.size())
          throw std::logic_error("event falls outside materialized schedule");
        epilogue.cycles[index].events.push_back(event);
      }
      ++explicitEvents;
    };
    auto appendStreamInstance = [&](const PeriodicStream& stream, std::uint64_t index) {
      std::int64_t logical = 0;
      if (!addSigned(stream.event.logicalIteration, static_cast<std::int64_t>(index), logical))
        throw std::overflow_error("event iteration provenance overflows signed 64-bit");
      Event event = stream.event;
      event.logicalIteration = logical;
      std::int64_t cycleSigned = 0;
      std::int64_t span = 0;
      if (!mulSigned(static_cast<std::int64_t>(index), ii, span) ||
          !addSigned(stream.firstLogicalTime, span, cycleSigned) || cycleSigned < 0)
        throw std::overflow_error("periodic event cycle overflows schedule");
      const auto cycle = static_cast<std::uint64_t>(cycleSigned);
      if (!hasKernel || cycle < kernelStartCycle || cycle >= kernelEndCycle)
        appendExplicit(event, cycle);
    };
    for (const auto& stream : streams) {
      const auto firstPeriod = static_cast<std::uint64_t>(stream.firstLogicalTime) / ii;
      if (hasKernel) {
        const auto kernelIndex = begin - firstPeriod;
        if (kernelIndex >= stream.count)
          throw std::logic_error("stream does not cover kernel begin");
        Event event = stream.event;
        event.logicalIteration += static_cast<std::int64_t>(kernelIndex);
        std::uint64_t cycleOffset = 0;
        std::uint64_t cycle = 0;
        if (!mulUnsigned(kernelIndex, ii, cycleOffset) ||
            !addUnsigned(static_cast<std::uint64_t>(stream.firstLogicalTime), cycleOffset, cycle))
          throw std::overflow_error("kernel event cycle overflows uint64");
        kernel.body[cycle % ii].events.push_back(std::move(event));
      }
      const auto before = hasKernel ? begin - firstPeriod : stream.count;
      std::uint64_t after = 0;
      if (hasKernel) {
        std::uint64_t streamEndExclusive = 0;
        std::uint64_t kernelEndExclusive = 0;
        if (!addUnsigned(firstPeriod, stream.count, streamEndExclusive) ||
            !addUnsigned(end, 1, kernelEndExclusive))
          throw std::overflow_error("periodic stream range overflows uint64");
        if (streamEndExclusive > kernelEndExclusive)
          after = streamEndExclusive - kernelEndExclusive;
      }
      std::uint64_t boundaryEvents = 0;
      if (!addUnsigned(before, after, boundaryEvents) ||
          !addUnsigned(plannedExplicitEvents, boundaryEvents, plannedExplicitEvents) ||
          plannedExplicitEvents > request.budget.maxExplicitBoundaryEvents)
        throw std::length_error("explicit periodic boundary event budget exceeded");
      for (std::uint64_t index = 0; index < before; ++index)
        appendStreamInstance(stream, index);
      if (hasKernel)
        for (std::uint64_t index = stream.count - after; index < stream.count; ++index)
          appendStreamInstance(stream, index);
    }
    for (const auto& shot : oneShots)
      appendExplicit(shot.event, static_cast<std::uint64_t>(shot.logicalTime));
    sortBundles(prologue);
    sortBundles(epilogue);
    sortKernel(kernel);
    result.stats.explicitEvents = explicitEvents;
    result.stats.explicitPrologueCycles = prologue.cycles.size();
    result.stats.explicitEpilogueCycles = epilogue.cycles.size();
    result.stats.kernelRepeatCount = kernel.repeatCount;
    result.schedule =
        MaterializedSchedule(ii, request.tripCount, shift, maxCycle, std::move(prologue),
                             std::move(kernel), std::move(epilogue));
    const auto verification =
        MaterializedScheduleVerifier::verify(dfg, target, mapping, request, *result.schedule);
    if (!verification.ok()) {
      result.status = ScheduleMaterializationStatus::VerificationFailure;
      add(result, ScheduleMaterializationDiagnosticCode::MAT_FINAL_VERIFICATION_FAILED,
          verification.format());
      result.schedule.reset();
      return result;
    }
    result.status = ScheduleMaterializationStatus::Success;
  } catch (const std::length_error& error) {
    result.status = ScheduleMaterializationStatus::MaterializationBudgetExceeded;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED,
        error.what());
  } catch (const std::overflow_error& error) {
    result.status = ScheduleMaterializationStatus::ArithmeticOverflow;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_TIME_ARITHMETIC_OVERFLOW, error.what());
  } catch (const std::invalid_argument& error) {
    result.status = ScheduleMaterializationStatus::InternalError;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INTERNAL_ERROR, error.what());
  } catch (const std::exception& error) {
    result.status = ScheduleMaterializationStatus::InternalError;
    add(result, ScheduleMaterializationDiagnosticCode::MAT_INTERNAL_ERROR, error.what());
  }
  return result;
}

} // namespace cgra::schedule
