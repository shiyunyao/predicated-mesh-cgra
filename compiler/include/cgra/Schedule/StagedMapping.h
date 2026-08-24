// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Schedule/StageConstraint.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cgra::schedule {

class StageScheduler;
class StageAssignmentVerifier;
class StagedMappingSerialization;
class StagedMappingTestAccess;

class StagedMapping {
public:
  const cgra::mapping::ModuloMapping& modulo() const noexcept { return modulo_; }
  PipelineStage stage(cgra::target::TargetNodeId node) const;
  std::uint64_t logicalIssueTime(cgra::target::TargetNodeId node) const;
  PipelineStage maxStage() const noexcept { return maxStage_; }
  std::span<const NodeStage> stages() const noexcept { return stages_; }

  friend bool operator==(const StagedMapping&, const StagedMapping&) = default;

private:
  friend class StageScheduler;
  friend class StageAssignmentVerifier;
  friend class StagedMappingSerialization;
  friend class StagedMappingTestAccess;

  StagedMapping(cgra::mapping::ModuloMapping modulo, std::vector<NodeStage> stages);

  cgra::mapping::ModuloMapping modulo_;
  std::vector<NodeStage> stages_;
  PipelineStage maxStage_ = 0;
};

} // namespace cgra::schedule
