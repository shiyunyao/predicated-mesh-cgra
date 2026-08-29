// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/PeriodicLifetime.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace cgra::register_allocation {
namespace {

bool boundaryReuseAllowed(cgra::SameAddressReadWritePolicy policy) {
  return policy == cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew;
}

bool intervalsConflict(std::int64_t lhsWrite, std::int64_t lhsRead, std::int64_t rhsWrite,
                       std::int64_t rhsRead, cgra::SameAddressReadWritePolicy policy) {
  const auto overlapStart = std::max(lhsWrite, rhsWrite);
  const auto overlapEnd = std::min(lhsRead, rhsRead);
  if (overlapStart < overlapEnd)
    return true;
  if (overlapStart == overlapEnd && !boundaryReuseAllowed(policy))
    return true;
  return false;
}

} // namespace

bool fixedRegisterSelfOverlaps(const StorageSegment& segment, std::uint32_t ii,
                               cgra::SameAddressReadWritePolicy policy) {
  if (ii == 0 || segment.readTime <= segment.writeTime)
    return true;
  const auto duration = segment.readTime - segment.writeTime;
  if (duration > ii)
    return true;
  if (duration < ii)
    return false;
  return !boundaryReuseAllowed(policy);
}

bool periodicLifetimesConflict(const StorageSegment& lhs, const StorageSegment& rhs,
                               std::uint32_t ii, cgra::SameAddressReadWritePolicy policy) {
  if (ii == 0 || lhs.readTime <= lhs.writeTime || rhs.readTime <= rhs.writeTime)
    return true;
  const auto lhsDuration = lhs.readTime - lhs.writeTime;
  const auto rhsDuration = rhs.readTime - rhs.writeTime;
  if (lhsDuration > ii || rhsDuration > ii)
    return true;
  const auto lhsWrite = static_cast<std::int64_t>(lhs.writeTime % ii);
  const auto rhsWrite = static_cast<std::int64_t>(rhs.writeTime % ii);
  const auto lhsRead = lhsWrite + static_cast<std::int64_t>(lhsDuration);
  const auto rhsRead = rhsWrite + static_cast<std::int64_t>(rhsDuration);
  const auto period = static_cast<std::int64_t>(ii);
  for (const auto shift : {-period, std::int64_t{0}, period}) {
    if (intervalsConflict(lhsWrite, lhsRead, rhsWrite + shift, rhsRead + shift, policy))
      return true;
  }
  return false;
}

bool phasePeriodicLifetimesConflict(const StorageSegment& lhs,
                                    std::span<const std::uint32_t> lhsRegisters,
                                    const StorageSegment& rhs,
                                    std::span<const std::uint32_t> rhsRegisters,
                                    std::uint32_t ii,
                                    cgra::SameAddressReadWritePolicy policy) {
  if (ii == 0 || lhsRegisters.empty() || rhsRegisters.empty() ||
      lhs.readTime <= lhs.writeTime || rhs.readTime <= rhs.writeTime)
    return true;
  const auto gcd = std::gcd(lhsRegisters.size(), rhsRegisters.size());
  const auto lhsPeriods = lhsRegisters.size() / gcd;
  if (lhsPeriods > std::numeric_limits<std::size_t>::max() / rhsRegisters.size())
    return true;
  const auto period = lhsPeriods * rhsRegisters.size();
  if (period == 0 || period > 100000)
    return true;

  // Compare one complete phase window against neighboring windows.  The
  // neighboring copies are needed when a lifetime crosses the window edge.
  const auto window = static_cast<std::int64_t>(period);
  for (std::int64_t lhsIteration = 0; lhsIteration < window; ++lhsIteration) {
    const auto lhsPhase = static_cast<std::size_t>(lhsIteration) % lhsRegisters.size();
    for (std::int64_t rhsIteration = -window; rhsIteration <= 2 * window; ++rhsIteration) {
      const auto rhsPhase = static_cast<std::size_t>(
          (rhsIteration % static_cast<std::int64_t>(rhsRegisters.size()) +
           static_cast<std::int64_t>(rhsRegisters.size())) %
          static_cast<std::int64_t>(rhsRegisters.size()));
      if (lhsRegisters[lhsPhase] != rhsRegisters[rhsPhase])
        continue;
      const auto lhsWrite = static_cast<std::int64_t>(lhs.writeTime) +
                            lhsIteration * static_cast<std::int64_t>(ii);
      const auto lhsRead = static_cast<std::int64_t>(lhs.readTime) +
                           lhsIteration * static_cast<std::int64_t>(ii);
      const auto rhsWrite = static_cast<std::int64_t>(rhs.writeTime) +
                            rhsIteration * static_cast<std::int64_t>(ii);
      const auto rhsRead = static_cast<std::int64_t>(rhs.readTime) +
                           rhsIteration * static_cast<std::int64_t>(ii);
      if (intervalsConflict(lhsWrite, lhsRead, rhsWrite, rhsRead, policy))
        return true;
    }
  }
  return false;
}

} // namespace cgra::register_allocation
