// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/StorageRequirement.h"

#include <cstdint>
#include <span>

namespace cgra::register_allocation {

bool fixedRegisterSelfOverlaps(
    const StorageSegment& segment, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy = cgra::SameAddressReadWritePolicy::Forbidden);

bool periodicLifetimesConflict(
    const StorageSegment& lhs, const StorageSegment& rhs, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy = cgra::SameAddressReadWritePolicy::Forbidden);

// Check two phase-expanded storage families over their least-common-multiple
// iteration window.  The register vectors are indexed by logical phase and
// contain physical register indices in the same RF bank.
bool phasePeriodicLifetimesConflict(
    const StorageSegment& lhs, std::span<const std::uint32_t> lhsRegisters,
    const StorageSegment& rhs, std::span<const std::uint32_t> rhsRegisters, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy = cgra::SameAddressReadWritePolicy::Forbidden);

} // namespace cgra::register_allocation
