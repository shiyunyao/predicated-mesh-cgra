// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocationBudget.h"
#include "cgra/RegisterAllocation/RotationFactorAnalysis.h"
#include "cgra/RegisterAllocation/StorageRequirement.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace cgra::register_allocation {

struct PhaseVertex {
  StorageSegmentId segment = 0;
  std::uint32_t phase = 0;
  friend bool operator==(const PhaseVertex&, const PhaseVertex&) = default;
  friend bool operator<(const PhaseVertex& lhs, const PhaseVertex& rhs) {
    return lhs.segment < rhs.segment ||
           (lhs.segment == rhs.segment && lhs.phase < rhs.phase);
  }
};

struct PhaseRegisterColoringResult {
  enum class Status {
    Success,
    RegisterDepthInfeasible,
    BudgetExceeded,
    InvalidRotationPlan,
  };

  Status status = Status::InvalidRotationPlan;
  std::map<PhaseVertex, std::uint32_t> colors;
  std::uint64_t decisions = 0;
  std::uint64_t backtracks = 0;
};

bool phaseVerticesConflict(const StorageSegment& lhs, std::uint32_t lhsPhase,
                           std::uint32_t lhsPhaseCount, const StorageSegment& rhs,
                           std::uint32_t rhsPhase, std::uint32_t rhsPhaseCount,
                           std::uint32_t ii, cgra::SameAddressReadWritePolicy policy);

PhaseRegisterColoringResult colorPhaseRegisters(
    std::span<const StorageSegment> segments, const RotationPlan& rotation,
    const TargetModel& target, const RFAllocationBudget& budget);

} // namespace cgra::register_allocation
