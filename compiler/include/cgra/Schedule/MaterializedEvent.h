// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/RegisterAllocation/RFAllocatedMapping.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace cgra::schedule {

enum class MaterializedEventKind {
  BoundaryValueInject,
  RFRead,
  NodeIssue,
  LinkLaunch,
  RFWrite,
  LiveOutBoundaryUse,
};

struct MaterializedEvent {
  MaterializedEventKind kind = MaterializedEventKind::NodeIssue;
  std::int64_t logicalIteration = 0;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<cgra::register_allocation::StorageSegmentId> segment;
  std::optional<cgra::ir::LiveOutId> liveOut;
  std::optional<std::uint32_t> transportActionIndex;
  std::optional<std::uint32_t> consumerIterationOffset;
  std::optional<cgra::ir::ExternalOperandBinding> boundaryValue;
  std::optional<cgra::mapping::NetworkDomain> domain;
  std::optional<cgra::mapping::TileCoord> tile;
  std::optional<cgra::mapping::Direction> direction;
  std::optional<cgra::register_allocation::PhysicalRegister> physicalRegister;

  friend bool operator==(const MaterializedEvent&, const MaterializedEvent&) = default;
};

std::string_view toString(MaterializedEventKind kind) noexcept;
MaterializedEventKind materializedEventKindFromString(std::string_view value);

} // namespace cgra::schedule
