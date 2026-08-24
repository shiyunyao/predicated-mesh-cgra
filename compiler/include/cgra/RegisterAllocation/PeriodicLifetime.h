// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/StorageRequirement.h"

#include <cstdint>

namespace cgra::register_allocation {

bool fixedRegisterSelfOverlaps(
    const StorageSegment& segment, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy = cgra::SameAddressReadWritePolicy::Forbidden);

bool periodicLifetimesConflict(
    const StorageSegment& lhs, const StorageSegment& rhs, std::uint32_t ii,
    cgra::SameAddressReadWritePolicy policy = cgra::SameAddressReadWritePolicy::Forbidden);

} // namespace cgra::register_allocation
