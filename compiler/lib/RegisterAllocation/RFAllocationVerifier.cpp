// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"

#include "cgra/RegisterAllocation/PeriodicLifetime.h"
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

namespace cgra::register_allocation {
namespace {

void add(RFAllocationVerificationReport& report, RFAllocationVerificationCode code,
         std::string message, std::optional<StorageSegmentId> segment = std::nullopt,
         std::optional<StorageSegmentId> conflicting = std::nullopt) {
  report.add({code, std::move(message), segment, conflicting});
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
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto& allocation = *allocations.at(segment.id);
    const auto* bank = target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
    auto& write = ports[{segment.tile, allocation.reg.bank,
                         static_cast<std::uint32_t>(segment.writeTime % ii)}];
    ++write.writes;
    auto& read = ports[{segment.tile, allocation.reg.bank,
                        static_cast<std::uint32_t>(segment.readTime % ii)}];
    ++read.reads;
    if (write.writes > bank->writePorts)
      add(report, RFAllocationVerificationCode::RFA_WRITE_PORT_CONFLICT,
          "allocated RF writes exceed target port capacity", segment.id);
    if (read.reads > bank->readPorts)
      add(report, RFAllocationVerificationCode::RFA_READ_PORT_CONFLICT,
          "allocated RF reads exceed target port capacity", segment.id);
  }

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
