// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Lowering/TargetLoweringResult.h"
#include "cgra/RegisterAllocation/RFAllocatedMapping.h"
#include "cgra/Schedule/MaterializedSchedule.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::lowering {

class TargetLowering {
public:
  static TargetLoweringResult lower(const target::TargetDFG& dfg, const TargetModel& target,
                                    const register_allocation::RFAllocatedMapping& mapping,
                                    const schedule::MaterializedSchedule& schedule,
                                    const TargetLoweringOptions& options = {});
};

class TargetControlProgramVerifier {
public:
  static bool verify(const target::TargetDFG& dfg, const TargetModel& target,
                     const register_allocation::RFAllocatedMapping& mapping,
                     const schedule::MaterializedSchedule& schedule,
                     const TargetControlProgram& program, std::string* error = nullptr);
};

} // namespace cgra::lowering
