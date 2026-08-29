// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"
#include "cgra/Transforms/RecurrenceIngressNormalization.h"

#include <string>
#include <vector>

namespace cgra::transforms {

struct RecurrenceIngressVerificationResult {
  bool ok = false;
  std::vector<std::string> diagnostics;

  std::string format() const;
};

RecurrenceIngressVerificationResult verifyRecurrenceIngress(
    const ir::DFG& original, const RecurrenceIngressNormalizationResult& normalized);

} // namespace cgra::transforms
