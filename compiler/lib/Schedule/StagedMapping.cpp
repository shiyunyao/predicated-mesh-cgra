// SPDX-License-Identifier: MIT
#include "cgra/Schedule/StagedMapping.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cgra::schedule {

StagedMapping::StagedMapping(cgra::mapping::ModuloMapping modulo, std::vector<NodeStage> stages)
    : modulo_(std::move(modulo)), stages_(std::move(stages)) {
  std::sort(stages_.begin(), stages_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.node < rhs.node; });
  for (std::size_t index = 1; index < stages_.size(); ++index)
    if (stages_[index - 1].node == stages_[index].node)
      throw std::invalid_argument("staged mapping contains duplicate node stage");
  for (const auto& entry : stages_)
    maxStage_ = std::max(maxStage_, entry.stage);
}

PipelineStage StagedMapping::stage(cgra::target::TargetNodeId node) const {
  const auto found =
      std::lower_bound(stages_.begin(), stages_.end(), node,
                       [](const auto& entry, const auto id) { return entry.node < id; });
  if (found == stages_.end() || found->node != node)
    throw std::out_of_range("staged mapping has no stage for target node");
  return found->stage;
}

std::uint64_t StagedMapping::logicalIssueTime(cgra::target::TargetNodeId node) const {
  const auto placement = modulo_.placement(node);
  const auto stageValue = stage(node);
  const auto ii = static_cast<std::uint64_t>(modulo_.ii());
  if (ii != 0 &&
      stageValue > (std::numeric_limits<std::uint64_t>::max() - placement.issueSlot.value()) / ii)
    throw std::overflow_error("staged mapping logical issue time overflows uint64");
  const auto wave = stageValue * ii;
  if (wave > std::numeric_limits<std::uint64_t>::max() - placement.issueSlot.value())
    throw std::overflow_error("staged mapping logical issue time overflows uint64");
  return wave + placement.issueSlot.value();
}

} // namespace cgra::schedule
