// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/StorageRequirement.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cgra::register_allocation {

enum class RotationAnalysisStatus {
  Success,
  InvalidII,
  RotationFactorOverflow,
  RotationPeriodOverflow,
  RotationPeriodExceedsControlMemory,
  RotationPhasesExceedRFDepth,
};

struct SegmentRotationRequirement {
  StorageSegmentId segment = 0;
  std::uint32_t minimumPhaseCount = 1;
  std::uint64_t duration = 0;
  std::uint32_t ii = 0;
};

struct RotationPlan {
  RotationAnalysisStatus status = RotationAnalysisStatus::Success;
  std::string diagnostic;
  std::vector<SegmentRotationRequirement> segments;
  std::uint32_t rotationPeriodIterations = 1;
  std::uint32_t controlPeriodCycles = 0;

  bool ok() const noexcept { return status == RotationAnalysisStatus::Success; }
};

RotationPlan analyzeRotationFactors(
    std::span<const StorageSegment> segments, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy, unsigned controlMemoryDepth = 0,
    unsigned maxRotationFactor = 0, unsigned maxControlPeriodCycles = 0);

} // namespace cgra::register_allocation
