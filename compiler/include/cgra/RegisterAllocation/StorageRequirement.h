// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/TileCoord.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cgra::register_allocation {

using StorageSegmentId = std::uint32_t;

enum class StorageOriginKind {
  ExplicitVirtualHold,
  TerminalSlack,
};

struct StorageOrigin {
  StorageOriginKind kind = StorageOriginKind::ExplicitVirtualHold;
  cgra::target::TargetEdgeId edge = 0;
  std::optional<std::uint32_t> transportActionIndex;

  friend bool operator==(const StorageOrigin&, const StorageOrigin&) = default;
};

struct StorageSegment {
  StorageSegmentId id = 0;
  cgra::target::TargetEdgeId edge = 0;
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankDomain domain = cgra::RegisterBankDomain::Data;
  std::uint64_t writeTime = 0;
  std::uint64_t readTime = 0;
  std::vector<StorageOrigin> origins;

  std::uint64_t duration() const noexcept { return readTime - writeTime; }
  friend bool operator==(const StorageSegment&, const StorageSegment&) = default;
};

class StorageRequirements {
public:
  std::uint32_t ii() const noexcept { return ii_; }
  std::span<const StorageSegment> segments() const noexcept { return segments_; }
  const StorageSegment& segment(StorageSegmentId id) const;
  bool operator==(const StorageRequirements&) const noexcept;

private:
  friend class StorageRequirementAnalysis;
  friend class RFAllocatedMapping;
  friend class RFAllocationVerifier;
  friend class RFAllocatedMappingSerialization;
  friend class StorageRequirementTestAccess;
  StorageRequirements(std::uint32_t ii, std::vector<StorageSegment> segments)
      : ii_(ii), segments_(std::move(segments)) {}

  std::uint32_t ii_ = 0;
  std::vector<StorageSegment> segments_;
};

} // namespace cgra::register_allocation
