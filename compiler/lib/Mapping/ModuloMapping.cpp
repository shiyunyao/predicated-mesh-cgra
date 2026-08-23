// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMapping.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::mapping {

ModuloMapping::ModuloMapping(std::string targetName, std::uint32_t ii,
                             std::vector<NodePlacement> placements,
                             std::vector<MappedDependence> dependences)
    : targetName_(std::move(targetName)), ii_(ii), placements_(std::move(placements)),
      dependences_(std::move(dependences)) {}

const NodePlacement& ModuloMapping::placement(cgra::target::TargetNodeId node) const {
  const auto found = std::find_if(placements_.begin(), placements_.end(),
                                  [node](const auto& placement) { return placement.node == node; });
  if (found == placements_.end())
    throw std::out_of_range("mapping has no placement for target node");
  return *found;
}

const MappedDependence& ModuloMapping::dependence(cgra::target::TargetEdgeId edge) const {
  const auto found =
      std::find_if(dependences_.begin(), dependences_.end(),
                   [edge](const auto& dependence) { return dependence.edge == edge; });
  if (found == dependences_.end())
    throw std::out_of_range("mapping has no realization for target edge");
  return *found;
}

bool ModuloMapping::operator==(const ModuloMapping& other) const noexcept {
  return targetName_ == other.targetName_ && ii_ == other.ii_ && placements_ == other.placements_ &&
         dependences_ == other.dependences_;
}

ModuloMappingBuilder::ModuloMappingBuilder(const cgra::target::TargetDFG& dfg, std::uint32_t ii)
    : targetName_(dfg.targetName()), time_(ii), hasExpectedGraph_(true) {
  for (const auto& node : dfg.nodes())
    expectedNodes_.push_back(node.id);
  for (const auto& edge : dfg.edges())
    expectedEdges_.push_back(edge);
  std::sort(expectedNodes_.begin(), expectedNodes_.end());
  std::sort(expectedEdges_.begin(), expectedEdges_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
}

ModuloMappingBuilder::ModuloMappingBuilder(std::string targetName, std::uint32_t ii)
    : targetName_(std::move(targetName)), time_(ii) {}

void ModuloMappingBuilder::place(cgra::target::TargetNodeId node, TileCoord tile,
                                 ModuloSlot issueSlot) {
  time_.validate(issueSlot);
  if (hasExpectedGraph_ && !std::binary_search(expectedNodes_.begin(), expectedNodes_.end(), node))
    throw std::invalid_argument("placement references unknown target node");
  if (std::ranges::any_of(placements_, [node](const auto& value) { return value.node == node; }))
    throw std::invalid_argument("target node has duplicate placement");
  placements_.push_back({node, tile, issueSlot});
}

void ModuloMappingBuilder::setTransport(cgra::target::TargetEdgeId edge, TransportPlan transport) {
  if (hasExpectedGraph_) {
    const auto found = std::find_if(expectedEdges_.begin(), expectedEdges_.end(),
                                    [edge](const auto& value) { return value.id == edge; });
    if (found == expectedEdges_.end())
      throw std::invalid_argument("transport references unknown target edge");
    if (found->kind() == cgra::ir::Edge::Kind::Memory)
      throw std::invalid_argument("memory edge cannot carry a transport plan");
    if (transport.domain != (found->kind() == cgra::ir::Edge::Kind::Predicate
                                 ? NetworkDomain::Predicate
                                 : NetworkDomain::Data))
      throw std::invalid_argument("transport domain does not match edge kind");
  }
  if (transport.edge != edge)
    throw std::invalid_argument("transport edge ID does not match mapping edge");
  if (std::ranges::any_of(dependences_, [edge](const auto& value) { return value.edge == edge; }))
    throw std::invalid_argument("target edge has duplicate mapping realization");
  for (const auto& action : transport.actions) {
    const auto actionDomain = std::visit([](const auto& value) { return value.domain; }, action);
    if (actionDomain != transport.domain)
      throw std::invalid_argument("transport action domain does not match transport plan");
    if (const auto* hold = std::get_if<VirtualHold>(&action);
        hold && hold->releaseElapsed <= hold->captureElapsed)
      throw std::invalid_argument("virtual hold release must be after capture");
  }
  MappedDependence dependence;
  dependence.edge = edge;
  dependence.kind =
      hasExpectedGraph_
          ? std::find_if(expectedEdges_.begin(), expectedEdges_.end(),
                         [edge](const auto& value) { return value.id == edge; })
                ->kind()
          : (transport.domain == NetworkDomain::Predicate ? cgra::ir::Edge::Kind::Predicate
                                                          : cgra::ir::Edge::Kind::Data);
  dependence.requiredSeparationCycles = transport.requiredSeparationCycles;
  dependence.transport = std::move(transport);
  dependences_.push_back(std::move(dependence));
}

void ModuloMappingBuilder::setMemorySeparation(cgra::target::TargetEdgeId edge,
                                               std::uint32_t cycles) {
  if (hasExpectedGraph_) {
    const auto found = std::find_if(expectedEdges_.begin(), expectedEdges_.end(),
                                    [edge](const auto& value) { return value.id == edge; });
    if (found == expectedEdges_.end())
      throw std::invalid_argument("memory separation references unknown target edge");
    if (found->kind() != cgra::ir::Edge::Kind::Memory)
      throw std::invalid_argument("only memory edges use memory separation");
  }
  if (std::ranges::any_of(dependences_, [edge](const auto& value) { return value.edge == edge; }))
    throw std::invalid_argument("target edge has duplicate mapping realization");
  dependences_.push_back({edge, cgra::ir::Edge::Kind::Memory, cycles, std::nullopt});
}

ModuloMapping ModuloMappingBuilder::finish() {
  if (hasExpectedGraph_) {
    if (placements_.size() != expectedNodes_.size() || dependences_.size() != expectedEdges_.size())
      throw std::invalid_argument("completed modulo mapping is missing nodes or dependences");
  }
  std::sort(placements_.begin(), placements_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.node < rhs.node; });
  std::sort(dependences_.begin(), dependences_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.edge < rhs.edge; });
  return ModuloMapping(targetName_, time_.ii(), std::move(placements_), std::move(dependences_));
}

} // namespace cgra::mapping
