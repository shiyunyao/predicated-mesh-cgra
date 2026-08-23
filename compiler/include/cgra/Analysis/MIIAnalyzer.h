// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Analysis/MIIResult.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::analysis {

class MIIAnalyzer {
public:
  static MIIResult analyze(const target::TargetDFG& dfg, const TargetModel& target);
};

} // namespace cgra::analysis
