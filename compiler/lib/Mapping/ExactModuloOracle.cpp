// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ExactModuloOracle.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Mapping/ModuloResourceModel.h"
#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace cgra::mapping {
namespace {

bool search(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
            ModuloResourceModel& resources, ResourceReservationTable& reservations,
            std::uint32_t ii, std::size_t index,
            std::map<cgra::target::TargetNodeId, NodePlacement>& placements,
            std::optional<ModuloMapping>& answer) {
  if (index == dfg.nodes().size()) {
    try {
      ModuloMappingBuilder builder(dfg, ii);
      for (const auto& [node, placement] : placements)
        builder.place(node, placement.tile, placement.issueSlot);
      for (const auto& edge : dfg.edges()) {
        if (edge.kind() != cgra::ir::Edge::Kind::Memory)
          return false;
        builder.setMemorySeparation(edge.id,
                                    target.memoryDependenceSeparation(
                                        std::get<cgra::ir::MemoryEdgeInfo>(edge.info).dependence));
      }
      auto candidate = builder.finish();
      if (ModuloMappingVerifier::verify(dfg, target, candidate).ok()) {
        answer = std::move(candidate);
        return true;
      }
    } catch (...) {
    }
    return false;
  }
  const auto& node = dfg.nodes()[index];
  for (const auto& [row, col] : target.compatibleTiles(node.operation)) {
    for (std::uint32_t slot = 0; slot < ii; ++slot) {
      const auto footprint = resources.operationFootprint(node, {row, col}, ModuloSlot(slot));
      const auto reservation = reservations.tryReserve(std::span<const ResourceId>(footprint),
                                                       {ReservationOwnerKind::Node, node.id});
      if (!reservation)
        continue;
      placements.emplace(node.id, NodePlacement{node.id, {row, col}, ModuloSlot(slot)});
      if (search(dfg, target, resources, reservations, ii, index + 1, placements, answer))
        return true;
      placements.erase(node.id);
      reservations.undo(*reservation);
    }
  }
  return false;
}

} // namespace

ExactOracleResult ExactModuloOracle::solve(const cgra::target::TargetDFG& dfg,
                                           const cgra::TargetModel& target, std::uint32_t ii,
                                           ExactOracleOptions options) {
  ExactOracleResult result;
  if (ii == 0 || ii > options.maxII || dfg.nodes().size() > options.maxNodes) {
    result.status = ExactOracleStatus::UnsupportedOracleSize;
    return result;
  }
  if (!cgra::target::TargetDFGVerifier::verify(dfg, target).ok()) {
    result.status = ExactOracleStatus::InvalidInput;
    return result;
  }
  if (std::ranges::any_of(dfg.edges(), [](const auto& edge) {
        return edge.kind() != cgra::ir::Edge::Kind::Memory;
      })) {
    result.status = ExactOracleStatus::UnsupportedOracleSize;
    return result;
  }
  ModuloResourceModel resources(target, ii);
  ResourceReservationTable reservations(resources);
  std::map<cgra::target::TargetNodeId, NodePlacement> placements;
  if (search(dfg, target, resources, reservations, ii, 0, placements, result.mapping))
    result.status = ExactOracleStatus::Feasible;
  else
    result.status = ExactOracleStatus::Infeasible;
  return result;
}

} // namespace cgra::mapping
