// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <functional>
#include <string>

namespace cgra::mapping {

// The modulo mapper deliberately treats later-stage feasibility as an opaque
// completion decision. This keeps placement/routing search independent from
// stage reconstruction and physical RF allocation.
enum class CompleteMappingDecision {
  Accept,
  Reject,
  Abort,
};

struct CompleteMappingCheckResult {
  CompleteMappingDecision decision = CompleteMappingDecision::Abort;
  std::string reasonCode;
  std::string message;
};

using CompleteMappingChecker = std::function<CompleteMappingCheckResult(
    const cgra::target::TargetDFG&, const cgra::TargetModel&, const ModuloMapping&)>;

} // namespace cgra::mapping
