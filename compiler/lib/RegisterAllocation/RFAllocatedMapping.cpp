// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocatedMapping.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::register_allocation {

RFAllocatedMapping::RFAllocatedMapping(cgra::schedule::StagedMapping staged,
                                       StorageRequirements requirements,
                                       std::vector<StorageAllocation> allocations)
    : staged_(std::move(staged)), requirements_(std::move(requirements)),
      allocations_(std::move(allocations)) {
  std::sort(allocations_.begin(), allocations_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.segment < rhs.segment; });
  for (std::size_t index = 1; index < allocations_.size(); ++index)
    if (allocations_[index - 1].segment == allocations_[index].segment)
      throw std::invalid_argument("RF allocation contains duplicate storage segment");
}

PhysicalRegister RFAllocatedMapping::registerFor(StorageSegmentId segment) const {
  return allocationFor(segment).reg;
}

const StorageAllocation& RFAllocatedMapping::allocationFor(StorageSegmentId segment) const {
  const auto found = std::lower_bound(
      allocations_.begin(), allocations_.end(), segment,
      [](const auto& allocation, const auto id) { return allocation.segment < id; });
  if (found == allocations_.end() || found->segment != segment)
    throw std::out_of_range("storage segment has no physical register allocation");
  return *found;
}

std::optional<PhysicalRegister>
RFAllocatedMapping::registerForVirtualHold(cgra::target::TargetEdgeId edge,
                                           std::uint32_t transportActionIndex) const {
  for (const auto& segment : requirements_.segments()) {
    for (const auto& origin : segment.origins) {
      if (origin.kind == StorageOriginKind::ExplicitVirtualHold && origin.edge == edge &&
          origin.transportActionIndex && *origin.transportActionIndex == transportActionIndex)
        return registerFor(segment.id);
    }
  }
  return std::nullopt;
}

std::optional<PhysicalRegister>
RFAllocatedMapping::registerForTerminalSlack(cgra::target::TargetEdgeId edge) const {
  for (const auto& segment : requirements_.segments()) {
    for (const auto& origin : segment.origins) {
      if (origin.kind == StorageOriginKind::TerminalSlack && origin.edge == edge)
        return registerFor(segment.id);
    }
  }
  return std::nullopt;
}

bool RFAllocatedMapping::operator==(const RFAllocatedMapping& other) const noexcept {
  return staged_ == other.staged_ && requirements_ == other.requirements_ &&
         allocations_ == other.allocations_;
}

} // namespace cgra::register_allocation
