// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloTime.h"
#include "cgra/Mapping/Network.h"
#include "cgra/Target/TargetDFG.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace cgra::mapping {

struct NodePlacement {
  cgra::target::TargetNodeId node = 0;
  TileCoord tile;
  ModuloSlot issueSlot;
  friend bool operator==(const NodePlacement&, const NodePlacement&) = default;
};

struct LinkStep {
  NetworkDomain domain = NetworkDomain::Data;
  TileCoord source;
  Direction direction = Direction::North;
  std::uint32_t elapsedFromProducerIssue = 0;
  friend bool operator==(const LinkStep&, const LinkStep&) = default;
};

struct VirtualHold {
  NetworkDomain domain = NetworkDomain::Data;
  TileCoord tile;
  std::uint32_t captureElapsed = 0;
  std::uint32_t releaseElapsed = 0;
  friend bool operator==(const VirtualHold&, const VirtualHold&) = default;
};

using TransportAction = std::variant<LinkStep, VirtualHold>;

struct TransportPlan {
  cgra::target::TargetEdgeId edge = 0;
  NetworkDomain domain = NetworkDomain::Data;
  std::vector<TransportAction> actions;
  std::uint32_t requiredSeparationCycles = 0;
  friend bool operator==(const TransportPlan&, const TransportPlan&) = default;
};

struct MappedDependence {
  cgra::target::TargetEdgeId edge = 0;
  cgra::ir::Edge::Kind kind = cgra::ir::Edge::Kind::Data;
  std::uint32_t requiredSeparationCycles = 0;
  std::optional<TransportPlan> transport;
  friend bool operator==(const MappedDependence&, const MappedDependence&) = default;
};

class ModuloMappingBuilder;
class UncheckedModuloMappingBuilder;

class ModuloMapping {
public:
  std::uint32_t ii() const noexcept { return ii_; }
  const std::string& targetName() const noexcept { return targetName_; }
  std::span<const NodePlacement> placements() const noexcept { return placements_; }
  std::span<const MappedDependence> dependences() const noexcept { return dependences_; }
  const NodePlacement& placement(cgra::target::TargetNodeId node) const;
  const MappedDependence& dependence(cgra::target::TargetEdgeId edge) const;
  bool operator==(const ModuloMapping& other) const noexcept;

private:
  friend class ModuloMappingBuilder;
  friend class UncheckedModuloMappingBuilder;
  friend class ModuloMappingTestAccess;
  ModuloMapping(std::string targetName, std::uint32_t ii, std::vector<NodePlacement> placements,
                std::vector<MappedDependence> dependences);

  std::string targetName_;
  std::uint32_t ii_;
  std::vector<NodePlacement> placements_;
  std::vector<MappedDependence> dependences_;
};

class ModuloMappingBuilder {
public:
  ModuloMappingBuilder(const cgra::target::TargetDFG& dfg, std::uint32_t ii);

  void place(cgra::target::TargetNodeId node, TileCoord tile, ModuloSlot issueSlot);
  void setTransport(cgra::target::TargetEdgeId edge, TransportPlan transport);
  void setMemorySeparation(cgra::target::TargetEdgeId edge, std::uint32_t cycles);
  ModuloMapping finish();

private:
  struct UncheckedTag {};
  friend class UncheckedModuloMappingBuilder;
  ModuloMappingBuilder(std::string targetName, std::uint32_t ii, UncheckedTag);

  std::string targetName_;
  ModuloTimeDomain time_;
  std::vector<cgra::target::TargetNodeId> expectedNodes_;
  std::vector<cgra::target::TargetEdge> expectedEdges_;
  std::vector<NodePlacement> placements_;
  std::vector<MappedDependence> dependences_;
  bool hasExpectedGraph_ = false;
};

// This builder is intentionally named and isolated for debug deserialization and
// malformed-graph tests. Production mapping code must use the graph-aware builder.
class UncheckedModuloMappingBuilder {
public:
  UncheckedModuloMappingBuilder(std::string targetName, std::uint32_t ii)
      : builder_(std::move(targetName), ii, ModuloMappingBuilder::UncheckedTag{}) {}

  void place(cgra::target::TargetNodeId node, TileCoord tile, ModuloSlot issueSlot) {
    builder_.place(node, tile, issueSlot);
  }
  void setTransport(cgra::target::TargetEdgeId edge, TransportPlan transport) {
    builder_.setTransport(edge, std::move(transport));
  }
  void setMemorySeparation(cgra::target::TargetEdgeId edge, std::uint32_t cycles) {
    builder_.setMemorySeparation(edge, cycles);
  }
  ModuloMapping finish() { return builder_.finish(); }

private:
  ModuloMappingBuilder builder_;
};

} // namespace cgra::mapping
