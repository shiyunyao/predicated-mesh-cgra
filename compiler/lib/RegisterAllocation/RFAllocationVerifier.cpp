// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"

#include "cgra/RegisterAllocation/PeriodicLifetime.h"
#include "cgra/RegisterAllocation/RFPortMatcher.h"
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace cgra::register_allocation {
namespace {

void add(RFAllocationVerificationReport& report, RFAllocationVerificationCode code,
         std::string message, std::optional<StorageSegmentId> segment = std::nullopt,
         std::optional<StorageSegmentId> conflicting = std::nullopt) {
  report.add({code, std::move(message), segment, conflicting});
}

std::string incomingSource(cgra::mapping::Direction direction, cgra::RegisterBankDomain domain) {
  const auto incoming = cgra::mapping::opposite(direction);
  const char* name = incoming == cgra::mapping::Direction::North   ? "NORTH"
                     : incoming == cgra::mapping::Direction::South ? "SOUTH"
                     : incoming == cgra::mapping::Direction::East  ? "EAST"
                                                                   : "WEST";
  return std::string(name) + (domain == cgra::RegisterBankDomain::Data ? "_DATA_IN" : "_PRED_IN");
}

std::string resultSource(cgra::TargetResultSource source) {
  switch (source) {
  case cgra::TargetResultSource::FuDataResult:
    return "FU_DATA_RESULT";
  case cgra::TargetResultSource::FuPredicateResult:
    return "FU_PRED_RESULT";
  case cgra::TargetResultSource::LsuLoadData:
    return "LSU_LOAD_DATA";
  case cgra::TargetResultSource::None:
    return {};
  }
  return {};
}

std::string storageWriteSource(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                               const cgra::schedule::StagedMapping& mapping,
                               const StorageSegment& segment) {
  const auto& edge = dfg.edge(segment.edge);
  const auto& transport = mapping.modulo().dependence(edge.id).transport;
  if (!transport)
    return {};
  for (const auto& origin : segment.origins) {
    if (origin.kind != StorageOriginKind::ExplicitVirtualHold || !origin.transportActionIndex)
      continue;
    const auto index = *origin.transportActionIndex;
    if (index > 0 && std::holds_alternative<cgra::mapping::LinkStep>(transport->actions[index - 1]))
      return incomingSource(
          std::get<cgra::mapping::LinkStep>(transport->actions[index - 1]).direction,
          segment.domain);
    return resultSource(target.operation(dfg.node(edge.src).operation).resultSource);
  }
  if (!transport->actions.empty() &&
      std::holds_alternative<cgra::mapping::LinkStep>(transport->actions.back()))
    return incomingSource(std::get<cgra::mapping::LinkStep>(transport->actions.back()).direction,
                          segment.domain);
  return {};
}

std::string boundaryWriteSource(const cgra::target::TargetDFG& dfg, const StorageSegment& segment) {
  const auto& edge = dfg.edge(segment.edge);
  if (edge.distance == 0)
    return {};
  const auto* boundary = edge.kind() == cgra::ir::Edge::Kind::Data
                             ? &std::get<cgra::ir::DataEdgeInfo>(edge.info).boundary
                             : &std::get<cgra::ir::PredicateEdgeInfo>(edge.info).boundary;
  if (!*boundary || (*boundary)->values.empty())
    return {};
  const auto& value = (*boundary)->values.front().value;
  if (!std::holds_alternative<cgra::ir::ConstantRef>(value))
    return {};
  if (edge.kind() == cgra::ir::Edge::Kind::Data)
    return "CONST_DATA";
  const auto constantId = std::get<cgra::ir::ConstantRef>(value).value;
  const auto constant =
      std::find_if(dfg.constants().begin(), dfg.constants().end(),
                   [constantId](const auto& item) { return item.id == constantId; });
  if (constant == dfg.constants().end())
    return {};
  return constant->bits != 0 ? "CONST_TRUE" : "CONST_FALSE";
}

} // namespace

bool RFAllocationVerificationReport::contains(RFAllocationVerificationCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string RFAllocationVerificationReport::format() const {
  std::ostringstream output;
  for (const auto& diagnostic : diagnostics_) {
    output << diagnostic.message;
    if (diagnostic.segment)
      output << " segment=" << *diagnostic.segment;
    if (diagnostic.conflictingSegment)
      output << " conflicting_segment=" << *diagnostic.conflictingSegment;
    output << '\n';
  }
  return output.str();
}

RFAllocationVerificationReport RFAllocationVerifier::verify(const cgra::target::TargetDFG& dfg,
                                                            const cgra::TargetModel& target,
                                                            const RFAllocatedMapping& mapping) {
  RFAllocationVerificationReport report;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    add(report, RFAllocationVerificationCode::RFA_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return report;
  }
  const auto stageReport =
      cgra::schedule::StageAssignmentVerifier::verify(dfg, target, mapping.staged());
  if (!stageReport.ok()) {
    add(report, RFAllocationVerificationCode::RFA_INVALID_STAGED_MAPPING,
        "StagedMapping precondition failed: " + stageReport.format());
    return report;
  }
  const auto requirementsResult =
      StorageRequirementAnalysis::analyze(dfg, target, mapping.staged());
  if (!requirementsResult.ok()) {
    add(report, RFAllocationVerificationCode::RFA_STORAGE_TIMING_INVALID,
        "storage requirement reconstruction failed: " + requirementsResult.format());
    return report;
  }
  if (*requirementsResult.requirements != mapping.storageRequirements()) {
    add(report, RFAllocationVerificationCode::RFA_STORAGE_TIMING_INVALID,
        "stored storage requirements differ from deterministic reconstruction");
    return report;
  }

  std::map<StorageSegmentId, const StorageAllocation*> allocations;
  for (const auto& allocation : mapping.allocations()) {
    if (!std::any_of(
            mapping.storageRequirements().segments().begin(),
            mapping.storageRequirements().segments().end(),
            [&allocation](const auto& segment) { return segment.id == allocation.segment; })) {
      add(report, RFAllocationVerificationCode::RFA_UNKNOWN_STORAGE_SEGMENT,
          "allocation references an unknown storage segment", allocation.segment);
      continue;
    }
    if (!allocations.emplace(allocation.segment, &allocation).second)
      add(report, RFAllocationVerificationCode::RFA_DUPLICATE_STORAGE_ALLOCATION,
          "storage segment has duplicate physical allocations", allocation.segment);
  }
  for (const auto& segment : mapping.storageRequirements().segments())
    if (!allocations.contains(segment.id))
      add(report, RFAllocationVerificationCode::RFA_UNALLOCATED_STORAGE_SEGMENT,
          "storage segment has no physical allocation", segment.id);
  if (!report.ok())
    return report;

  const auto ii = mapping.storageRequirements().ii();
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto& allocation = *allocations.at(segment.id);
    const auto* bank = target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
    if (!bank) {
      add(report, RFAllocationVerificationCode::RFA_BANK_DOMAIN_MISMATCH,
          "allocated segment has no applicable target bank", segment.id);
      continue;
    }
    if (allocation.reg.tile != segment.tile || allocation.reg.bank != bank->id ||
        bank->domain != segment.domain) {
      add(report, RFAllocationVerificationCode::RFA_BANK_DOMAIN_MISMATCH,
          "physical allocation bank or tile disagrees with storage segment", segment.id);
    }
    if (!bank->allocates(allocation.reg.index))
      add(report, RFAllocationVerificationCode::RFA_INVALID_REGISTER_INDEX,
          "physical allocation index is not allocatable in target bank", segment.id);
    if (fixedRegisterSelfOverlaps(segment, ii, bank->sameAddressReadWritePolicy))
      add(report, RFAllocationVerificationCode::RFA_FIXED_REGISTER_SELF_OVERLAP,
          "storage lifetime overlaps its fixed-register next iteration", segment.id);
  }
  if (!report.ok())
    return report;

  struct PortCounts {
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;
  };
  struct PortKey {
    cgra::mapping::TileCoord tile;
    cgra::RegisterBankId bank;
    std::uint32_t slot = 0;
    bool operator<(const PortKey& other) const {
      return std::tie(tile.row, tile.col, bank, slot) <
             std::tie(other.tile.row, other.tile.col, other.bank, other.slot);
    }
  };
  std::map<PortKey, PortCounts> ports;
  std::map<PortKey, std::set<std::uint32_t>> readAssignments;
  std::map<PortKey, std::set<std::uint32_t>> writeAssignments;
  std::map<PortKey, std::vector<RFPortEvent>> readEvents;
  std::map<PortKey, std::vector<RFPortEvent>> writeEvents;
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto& allocation = *allocations.at(segment.id);
    const auto* bank = target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
    auto& write = ports[{segment.tile, allocation.reg.bank,
                         static_cast<std::uint32_t>(segment.writeTime % ii)}];
    ++write.writes;
    auto& read = ports[{segment.tile, allocation.reg.bank,
                        static_cast<std::uint32_t>(segment.readTime % ii)}];
    ++read.reads;
    RFPortEvent readEvent;
    readEvent.id = segment.id;
    readEvent.kind = RFPortEventKind::PeriodicRead;
    readEvent.tile = segment.tile;
    readEvent.domain = segment.domain;
    readEvent.slot = static_cast<std::uint32_t>(segment.readTime % ii);
    readEvent.segment = segment.id;
    readEvents[{segment.tile, allocation.reg.bank,
                static_cast<std::uint32_t>(segment.readTime % ii)}]
        .push_back(std::move(readEvent));
    RFPortEvent writeEvent;
    writeEvent.id = segment.id;
    writeEvent.kind = RFPortEventKind::PeriodicWrite;
    writeEvent.tile = segment.tile;
    writeEvent.domain = segment.domain;
    writeEvent.slot = static_cast<std::uint32_t>(segment.writeTime % ii);
    writeEvent.segment = segment.id;
    writeEvent.writeSource = storageWriteSource(dfg, target, mapping.staged(), segment);
    writeEvents[{segment.tile, allocation.reg.bank,
                 static_cast<std::uint32_t>(segment.writeTime % ii)}]
        .push_back(std::move(writeEvent));
    if (write.writes > bank->writePorts)
      add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
          "allocated RF writes exceed target port capacity", segment.id);
    if (read.reads > bank->readPorts)
      add(report, RFAllocationVerificationCode::RFA_READ_PORT_CONFLICT,
          "allocated RF reads exceed target port capacity", segment.id);
    if (allocation.readPort >= bank->readPorts ||
        !readAssignments[{segment.tile, allocation.reg.bank,
                          static_cast<std::uint32_t>(segment.readTime % ii)}]
             .insert(allocation.readPort)
             .second)
      add(report, RFAllocationVerificationCode::RFA_READ_PORT_CONFLICT,
          "storage segments share an exclusive RF read port", segment.id);
    if (allocation.writePort >= bank->writePorts ||
        !writeAssignments[{segment.tile, allocation.reg.bank,
                           static_cast<std::uint32_t>(segment.writeTime % ii)}]
             .insert(allocation.writePort)
             .second)
      add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
          "storage segments share an exclusive RF write port", segment.id);
    const auto source = storageWriteSource(dfg, target, mapping.staged(), segment);
    const auto sourceIt = bank->writePortSources.find("W" + std::to_string(allocation.writePort));
    if (sourceIt == bank->writePortSources.end() ||
        std::find(sourceIt->second.begin(), sourceIt->second.end(), source) ==
            sourceIt->second.end())
      add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
          "assigned RF write port does not accept the mapped value source", segment.id);
    const auto boundarySource = boundaryWriteSource(dfg, segment);
    if (!boundarySource.empty()) {
      if (!allocation.boundaryWritePort || *allocation.boundaryWritePort >= bank->writePorts)
        add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
            "recurrence boundary has no assigned physical RF write port", segment.id);
      else {
        const auto boundaryIt =
            bank->writePortSources.find("W" + std::to_string(*allocation.boundaryWritePort));
        if (boundaryIt == bank->writePortSources.end() ||
            std::find(boundaryIt->second.begin(), boundaryIt->second.end(), boundarySource) ==
                boundaryIt->second.end())
          add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
              "assigned recurrence-boundary RF write port does not accept the seed source",
              segment.id);
      }
    } else if (allocation.boundaryWritePort) {
      add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
          "allocation carries a boundary RF write port for an edge without a constant seed",
          segment.id);
    }
  }

  // Rebuild source-compatible matchings independently from the serialized
  // port numbers. The checks below then confirm that the allocation records
  // one of the valid assignments returned by the shared contract matcher.
  const auto verifyMatches = [&](const auto& events, bool writes) {
    for (const auto& [key, normalized] : events) {
      const auto domain = normalized.front().domain;
      const auto* bank = target.registerBank(domain, key.tile.row, key.tile.col);
      if (!bank)
        continue;
      const auto match = matchRFPorts(*bank, normalized);
      if (!match.ok())
        add(report, writes ? RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT
                           : RFAllocationVerificationCode::RFA_READ_PORT_CONFLICT,
            "shared RF port matcher rejects reconstructed access events");
    }
  };
  verifyMatches(readEvents, false);
  verifyMatches(writeEvents, true);

  struct RegisterEventCounts {
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;
  };
  using RegisterEventKey =
      std::tuple<cgra::mapping::TileCoord, cgra::RegisterBankId, std::uint32_t, std::uint32_t>;
  std::map<RegisterEventKey, RegisterEventCounts> registerEvents;
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto& allocation = *allocations.at(segment.id);
    ++registerEvents[{allocation.reg.tile, allocation.reg.bank, allocation.reg.index,
                      static_cast<std::uint32_t>(segment.writeTime % ii)}]
          .writes;
    ++registerEvents[{allocation.reg.tile, allocation.reg.bank, allocation.reg.index,
                      static_cast<std::uint32_t>(segment.readTime % ii)}]
          .reads;
  }
  for (const auto& [key, counts] : registerEvents) {
    const auto domain = std::get<1>(key) == target.predicateRF().id
                            ? cgra::RegisterBankDomain::Predicate
                            : cgra::RegisterBankDomain::Data;
    const auto* bank = target.registerBank(domain, std::get<0>(key).row, std::get<0>(key).col);
    if (counts.writes > 1)
      add(report, RFAllocationVerificationCode::RFA_SAME_ADDRESS_RW_CONFLICT,
          "multiple periodic writes target one physical RF register in one cycle");
    if (counts.reads != 0 && counts.writes != 0 && bank &&
        bank->sameAddressReadWritePolicy != cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew)
      add(report, RFAllocationVerificationCode::RFA_SAME_ADDRESS_RW_CONFLICT,
          "same-address periodic read/write is forbidden by target RF policy");
  }

  for (std::size_t lhsIndex = 0; lhsIndex < mapping.storageRequirements().segments().size();
       ++lhsIndex) {
    const auto& lhs = mapping.storageRequirements().segments()[lhsIndex];
    const auto& lhsAllocation = *allocations.at(lhs.id);
    for (std::size_t rhsIndex = lhsIndex + 1;
         rhsIndex < mapping.storageRequirements().segments().size(); ++rhsIndex) {
      const auto& rhs = mapping.storageRequirements().segments()[rhsIndex];
      const auto& rhsAllocation = *allocations.at(rhs.id);
      if (!(lhsAllocation.reg == rhsAllocation.reg))
        continue;
      const auto* bank = target.registerBank(lhs.domain, lhs.tile.row, lhs.tile.col);
      if (periodicLifetimesConflict(lhs, rhs, ii, bank->sameAddressReadWritePolicy))
        add(report, RFAllocationVerificationCode::RFA_PERIODIC_REGISTER_CONFLICT,
            "two storage lifetimes conflict under fixed-register periodic reuse", lhs.id, rhs.id);
    }
  }
  return report;
}

} // namespace cgra::register_allocation
