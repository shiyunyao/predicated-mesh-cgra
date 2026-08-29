// SPDX-License-Identifier: MIT
#include "cgra/Mapping/StageDifferenceAnalysis.h"

#include <limits>
#include <stdexcept>

namespace cgra::mapping {

std::int64_t ceilDivSigned(std::int64_t numerator, std::int64_t positiveDenominator) {
  if (positiveDenominator <= 0)
    throw std::invalid_argument("signed ceil division requires a positive denominator");
  const auto quotient = numerator / positiveDenominator;
  const auto remainder = numerator % positiveDenominator;
  // C++ truncates integer division toward zero.  Only positive numerators
  // need the quotient rounded upward for a mathematical ceiling.
  if (remainder != 0 && numerator > 0) {
    if (quotient == std::numeric_limits<std::int64_t>::max())
      throw std::overflow_error("signed ceil division overflows int64");
    return quotient + 1;
  }
  return quotient;
}

StageDifferenceRequirement minimumStageDifference(
    const cgra::target::TargetEdge& edge, const NodePlacement& source,
    const NodePlacement& destination, const MappedDependence& dependence, std::uint32_t ii) {
  if (ii == 0)
    throw std::invalid_argument("stage difference requires a non-zero II");
  const auto slotDifference = static_cast<std::int64_t>(destination.issueSlot.value()) -
                              static_cast<std::int64_t>(source.issueSlot.value());
  const auto numerator = static_cast<std::int64_t>(dependence.requiredSeparationCycles) -
                         slotDifference;
  auto delta = ceilDivSigned(numerator, static_cast<std::int64_t>(ii));
  const auto distance = static_cast<std::int64_t>(edge.distance);
  if (delta < distance && delta < std::numeric_limits<std::int64_t>::min() + distance)
    throw std::overflow_error("stage difference distance subtraction overflows int64");
  delta -= distance;
  return {edge.id, delta, edge.distance, dependence.requiredSeparationCycles};
}

std::uint64_t minimumTerminalSlackCycles(const StageDifferenceRequirement& requirement,
                                         const NodePlacement& source,
                                         const NodePlacement& destination, std::uint32_t ii) {
  if (ii == 0)
    throw std::invalid_argument("terminal slack requires a non-zero II");
  const auto slotDifference = static_cast<std::int64_t>(destination.issueSlot.value()) -
                              static_cast<std::int64_t>(source.issueSlot.value());
  const auto stageDistance = requirement.minimumStageDelta +
                             static_cast<std::int64_t>(requirement.distance);
  if (stageDistance != 0 &&
      (stageDistance > std::numeric_limits<std::int64_t>::max() / ii ||
       stageDistance < std::numeric_limits<std::int64_t>::min() / ii))
    throw std::overflow_error("terminal slack multiplication overflows int64");
  const auto stageCycles = stageDistance * static_cast<std::int64_t>(ii);
  if ((stageCycles > 0 && slotDifference > std::numeric_limits<std::int64_t>::max() - stageCycles) ||
      (stageCycles < 0 && slotDifference < std::numeric_limits<std::int64_t>::min() - stageCycles))
    throw std::overflow_error("terminal slack addition overflows int64");
  const auto partial = slotDifference + stageCycles;
  const auto required = static_cast<std::int64_t>(requirement.requiredSeparationCycles);
  if (required < 0 || (partial < std::numeric_limits<std::int64_t>::min() + required))
    throw std::overflow_error("terminal slack subtraction overflows int64");
  const auto slack = partial - required;
  if (slack < 0)
    throw std::overflow_error("terminal slack is outside uint64 range");
  return static_cast<std::uint64_t>(slack);
}

} // namespace cgra::mapping
