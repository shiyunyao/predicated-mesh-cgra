// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/StageSchedulingResult.h"
#include "cgra/Mapping/StageDifferenceAnalysis.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>

namespace cgra::schedule {

inline std::int64_t ceilDivSigned(std::int64_t numerator, std::int64_t positiveDenominator) {
  return cgra::mapping::ceilDivSigned(numerator, positiveDenominator);
}

class StageScheduler {
public:
  static StageSchedulingResult schedule(const cgra::target::TargetDFG& dfg,
                                        const cgra::TargetModel& target,
                                        const cgra::mapping::ModuloMapping& mapping);
};

} // namespace cgra::schedule
