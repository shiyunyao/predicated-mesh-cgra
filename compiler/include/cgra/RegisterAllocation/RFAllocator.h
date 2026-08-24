// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocationResult.h"
#include "cgra/Target/TargetDFG.h"

namespace cgra::register_allocation {

class RFAllocator {
public:
  static RFAllocationResult allocate(const cgra::target::TargetDFG& dfg,
                                     const cgra::TargetModel& target,
                                     const cgra::schedule::StagedMapping& mapping,
                                     const RFAllocationOptions& options = {});
};

} // namespace cgra::register_allocation
