// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Target/TargetDFG.h"

#include <cstdint>

namespace cgra::mapping {

struct StageDifferenceRequirement {
  cgra::target::TargetEdgeId edge = 0;
  std::int64_t minimumStageDelta = 0;
  std::uint32_t distance = 0;
  std::uint32_t requiredSeparationCycles = 0;
};

std::int64_t ceilDivSigned(std::int64_t numerator, std::int64_t positiveDenominator);

StageDifferenceRequirement minimumStageDifference(
    const cgra::target::TargetEdge& edge, const NodePlacement& source,
    const NodePlacement& destination, const MappedDependence& dependence, std::uint32_t ii);

std::uint64_t minimumTerminalSlackCycles(const StageDifferenceRequirement& requirement,
                                         const NodePlacement& source,
                                         const NodePlacement& destination, std::uint32_t ii);

} // namespace cgra::mapping
