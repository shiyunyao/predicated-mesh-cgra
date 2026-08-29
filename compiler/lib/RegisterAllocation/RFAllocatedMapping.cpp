// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocatedMapping.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
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
  const auto& allocation = allocationFor(segment);
  if (allocation.family.phaseCount != 1)
    throw std::logic_error(
        "periodic register family requires registerFor(segment, logicalIteration)");
  return allocation.reg;
}

PhysicalRegister RFAllocatedMapping::registerFor(StorageSegmentId segment,
                                                 std::int64_t logicalIteration) const {
  const auto& allocation = allocationFor(segment);
  if (allocation.family.phaseCount <= 1 || allocation.family.phases.empty())
    return allocation.reg;
  const auto phaseCount = static_cast<std::int64_t>(allocation.family.phaseCount);
  auto phase = logicalIteration % phaseCount;
  if (phase < 0)
    phase += phaseCount;
  for (const auto& entry : allocation.family.phases)
    if (entry.phase == static_cast<std::uint32_t>(phase))
      return entry.reg;
  throw std::logic_error("periodic register family is missing a phase allocation");
}

std::uint32_t RFAllocatedMapping::rotationPeriodIterations() const {
  std::uint64_t period = 1;
  for (const auto& allocation : allocations_) {
    const auto factor = allocation.family.phaseCount == 0 ? 1 : allocation.family.phaseCount;
    const auto gcd = std::gcd(period, static_cast<std::uint64_t>(factor));
    const auto scaled = period / gcd;
    if (scaled > std::numeric_limits<std::uint32_t>::max() / factor)
      throw std::overflow_error("rotation period exceeds uint32 range");
    period = scaled * factor;
  }
  return static_cast<std::uint32_t>(period);
}

std::uint32_t RFAllocatedMapping::controlPeriodCycles(std::uint32_t logicalII) const {
  const auto period = rotationPeriodIterations();
  if (logicalII != 0 && period > std::numeric_limits<std::uint32_t>::max() / logicalII)
    throw std::overflow_error("control period exceeds uint32 range");
  return period * logicalII;
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
        return registerFor(segment.id, 0);
    }
  }
  return std::nullopt;
}

std::optional<PhysicalRegister>
RFAllocatedMapping::registerForTerminalSlack(cgra::target::TargetEdgeId edge) const {
  for (const auto& segment : requirements_.segments()) {
    for (const auto& origin : segment.origins) {
      if (origin.kind == StorageOriginKind::TerminalSlack && origin.edge == edge)
        return registerFor(segment.id, 0);
    }
  }
  return std::nullopt;
}

bool RFAllocatedMapping::operator==(const RFAllocatedMapping& other) const noexcept {
  return staged_ == other.staged_ && requirements_ == other.requirements_ &&
         allocations_ == other.allocations_;
}

} // namespace cgra::register_allocation
