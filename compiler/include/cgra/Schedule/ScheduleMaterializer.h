// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"
#include "cgra/Schedule/ScheduleMaterializationResult.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::schedule {

class ScheduleMaterializer {
public:
  static ScheduleMaterializationResult
  materialize(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
              const cgra::register_allocation::RFAllocatedMapping& mapping,
              const ScheduleMaterializationRequest& request);
};

} // namespace cgra::schedule
