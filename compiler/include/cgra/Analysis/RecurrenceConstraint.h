// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/TargetDFG.h"

#include <cstdint>

namespace cgra::analysis {

struct RecurrenceConstraint {
  target::TargetEdgeId edge = 0;
  target::TargetNodeId src = 0;
  target::TargetNodeId dst = 0;
  std::uint32_t distance = 0;
  std::uint32_t intrinsicSeparation = 0;

  friend bool operator==(const RecurrenceConstraint&, const RecurrenceConstraint&) = default;
};

} // namespace cgra::analysis
