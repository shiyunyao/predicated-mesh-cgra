// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Mapping/ModuloMappingDiagnostic.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::mapping {

class ModuloMappingVerifier {
public:
  static ModuloMappingVerificationReport verify(const cgra::target::TargetDFG& dfg,
                                                const cgra::TargetModel& target,
                                                const ModuloMapping& mapping);
};

} // namespace cgra::mapping
