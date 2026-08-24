// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Lowering/TargetLoweringResult.h"
#include "cgra/Target/TargetDFG.h"

#include <string_view>

namespace cgra::lowering {

class ProgramManifestBuilder {
public:
  static ProgramManifest build(const target::TargetDFG& dfg, const TargetModel& target,
                               const TargetControlProgram& program,
                               const EncodedTargetProgram& encoded,
                               const TargetLoweringOptions& options = {});
};

} // namespace cgra::lowering
