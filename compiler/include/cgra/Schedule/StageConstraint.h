// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/TargetDFG.h"

#include <cstdint>

namespace cgra::schedule {

using PipelineStage = std::uint64_t;

struct StageConstraint {
  cgra::target::TargetEdgeId edge = 0;
  cgra::target::TargetNodeId src = 0;
  cgra::target::TargetNodeId dst = 0;
  std::int64_t minimumStageDelta = 0;
  std::uint32_t distance = 0;
  std::uint32_t requiredSeparationCycles = 0;

  friend bool operator==(const StageConstraint&, const StageConstraint&) = default;
};

struct NodeStage {
  cgra::target::TargetNodeId node = 0;
  PipelineStage stage = 0;

  friend bool operator==(const NodeStage&, const NodeStage&) = default;
};

} // namespace cgra::schedule
