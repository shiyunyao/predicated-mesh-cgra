// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RotationFactorAnalysis.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace cgra::register_allocation {
namespace {

std::uint32_t requiredPhases(const StorageSegment& segment, std::uint32_t ii,
                             cgra::SameAddressReadWritePolicy policy) {
  const auto duration = segment.duration();
  if (duration < ii)
    return 1;
  if (policy == cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew)
    return static_cast<std::uint32_t>((duration + ii - 1) / ii);
  return static_cast<std::uint32_t>(duration / ii + 1);
}

bool multiplyFits(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& output) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    return false;
  output = lhs * rhs;
  return true;
}

} // namespace

RotationPlan analyzeRotationFactors(std::span<const StorageSegment> segments, std::uint32_t ii,
                                    cgra::SameAddressReadWritePolicy policy,
                                    unsigned controlMemoryDepth, unsigned maxRotationFactor,
                                    unsigned maxControlPeriodCycles) {
  RotationPlan plan;
  if (ii == 0) {
    plan.status = RotationAnalysisStatus::InvalidII;
    plan.diagnostic = "rotation analysis requires a non-zero initiation interval";
    return plan;
  }
  std::uint64_t periodIterations = 1;
  for (const auto& segment : segments) {
    const auto factor = requiredPhases(segment, ii, policy);
    if (maxRotationFactor != 0 && factor > maxRotationFactor) {
      plan.status = RotationAnalysisStatus::RotationFactorOverflow;
      plan.diagnostic = "storage segment requires more phases than the configured rotation cap";
      return plan;
    }
    plan.segments.push_back({segment.id, factor, segment.duration(), ii});
    const auto gcd = std::gcd(static_cast<std::uint32_t>(periodIterations), factor);
    const auto quotient = periodIterations / gcd;
    if (quotient > std::numeric_limits<std::uint64_t>::max() / factor) {
      plan.status = RotationAnalysisStatus::RotationPeriodOverflow;
      plan.diagnostic = "rotation phase LCM overflows 64-bit arithmetic";
      return plan;
    }
    periodIterations = quotient * factor;
  }
  if (periodIterations > std::numeric_limits<std::uint32_t>::max()) {
    plan.status = RotationAnalysisStatus::RotationPeriodOverflow;
    plan.diagnostic = "rotation period does not fit the control-period representation";
    return plan;
  }
  std::uint64_t controlPeriod = 0;
  if (!multiplyFits(periodIterations, ii, controlPeriod) ||
      controlPeriod > std::numeric_limits<std::uint32_t>::max()) {
    plan.status = RotationAnalysisStatus::RotationPeriodOverflow;
    plan.diagnostic = "control period overflows 32-bit schedule metadata";
    return plan;
  }
  if ((controlMemoryDepth != 0 && controlPeriod > controlMemoryDepth) ||
      (maxControlPeriodCycles != 0 && controlPeriod > maxControlPeriodCycles)) {
    plan.status = RotationAnalysisStatus::RotationPeriodExceedsControlMemory;
    plan.diagnostic = "rotation control period exceeds target control memory";
    return plan;
  }
  plan.rotationPeriodIterations = static_cast<std::uint32_t>(periodIterations);
  plan.controlPeriodCycles = static_cast<std::uint32_t>(controlPeriod);
  return plan;
}

} // namespace cgra::register_allocation
