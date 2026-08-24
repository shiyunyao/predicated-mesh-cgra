// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ExactModuloOracle.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Mapping/ModuloResourceModel.h"
#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace cgra::mapping {
namespace {

struct RouteState {
  TileCoord tile;
  ModuloSlot slot;
  std::uint32_t elapsed = 0;
  std::vector<TransportAction> actions;
  std::vector<ResourceId> linkResources;
  std::set<std::size_t> visited;
};

using RouteConsumer = std::function<bool(const TransportPlan&, const std::vector<ResourceId>&)>;

std::size_t stateId(TileCoord tile, ModuloSlot slot, const cgra::TargetModel& target) {
  return (static_cast<std::size_t>(slot.value()) * target.array().rows + tile.row) *
             target.array().cols +
         tile.col;
}

NetworkDomain domainFor(cgra::ir::Edge::Kind kind) {
  return kind == cgra::ir::Edge::Kind::Predicate ? NetworkDomain::Predicate : NetworkDomain::Data;
}

bool routeCandidates(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                     const ModuloResourceModel& resources,
                     const ResourceReservationTable& reservations, std::uint32_t ii,
                     const cgra::target::TargetEdge& edge, const NodePlacement& producer,
                     const NodePlacement& consumer, const RouteConsumer& consume) {
  const auto domain = domainFor(edge.kind());
  const auto& network =
      domain == NetworkDomain::Predicate ? target.predicateNetwork() : target.dataNetwork();
  const auto ready = dfg.node(edge.src).producerOutputReadyOffset.value_or(0U);
  const ModuloTimeDomain time(ii);
  const auto stateCount = static_cast<std::size_t>(target.array().rows) * target.array().cols * ii;
  if (stateCount == 0)
    return false;

  std::function<bool(const RouteState&)> visit = [&](const RouteState& current) {
    if (current.tile == consumer.tile) {
      if (current.actions.empty()) {
        if (producer.tile == consumer.tile && ready != std::numeric_limits<std::uint32_t>::max()) {
          TransportPlan plan{
              edge.id, domain, {VirtualHold{domain, producer.tile, ready, ready + 1}}, ready + 1};
          if (consume(plan, {}))
            return true;
        }
      } else {
        if (current.elapsed <= std::numeric_limits<std::uint32_t>::max()) {
          TransportPlan plan{edge.id, domain, current.actions, current.elapsed};
          if (consume(plan, current.linkResources))
            return true;
        }
      }
    }

    for (const auto direction :
         {Direction::North, Direction::East, Direction::South, Direction::West}) {
      const auto nextTile = neighbor(current.tile, direction, target);
      if (!nextTile)
        continue;
      const auto launchSlot = time.advance(producer.issueSlot, current.elapsed);
      const auto resource = resources.linkResource(domain, current.tile, direction, launchSlot);
      if (!resource || !reservations.isFree(*resource) ||
          std::ranges::find(current.linkResources, *resource) != current.linkResources.end())
        continue;
      const auto nextSlot = time.advance(current.slot, network.hopLatency);
      const auto nextId = stateId(*nextTile, nextSlot, target);
      if (current.visited.contains(nextId))
        continue;
      if (current.elapsed > std::numeric_limits<std::uint32_t>::max() - network.hopLatency)
        continue;
      RouteState next = current;
      next.tile = *nextTile;
      next.slot = nextSlot;
      next.elapsed += network.hopLatency;
      next.actions.emplace_back(LinkStep{domain, current.tile, direction, current.elapsed});
      next.linkResources.push_back(*resource);
      next.visited.insert(nextId);
      if (visit(next))
        return true;
    }

    const auto nextSlot = time.advance(current.slot, 1);
    const auto nextId = stateId(current.tile, nextSlot, target);
    if (!current.visited.contains(nextId)) {
      if (current.elapsed != std::numeric_limits<std::uint32_t>::max()) {
        RouteState next = current;
        next.slot = nextSlot;
        ++next.elapsed;
        if (!next.actions.empty()) {
          if (auto* hold = std::get_if<VirtualHold>(&next.actions.back());
              hold && hold->domain == domain && hold->tile == current.tile &&
              hold->releaseElapsed == current.elapsed) {
            hold->releaseElapsed = next.elapsed;
          } else {
            next.actions.emplace_back(
                VirtualHold{domain, current.tile, current.elapsed, next.elapsed});
          }
        } else {
          next.actions.emplace_back(
              VirtualHold{domain, current.tile, current.elapsed, next.elapsed});
        }
        next.visited.insert(nextId);
        if (visit(next))
          return true;
      }
    }
    return false;
  };

  const auto startSlot = time.advance(producer.issueSlot, ready);
  RouteState start{producer.tile, startSlot, ready, {}, {}, {}};
  start.visited.insert(stateId(start.tile, start.slot, target));
  return visit(start);
}

bool realizeEdges(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                  ModuloResourceModel& resources, ResourceReservationTable& reservations,
                  std::uint32_t ii, std::size_t edgeIndex,
                  const std::map<cgra::target::TargetNodeId, NodePlacement>& placements,
                  std::map<cgra::target::TargetEdgeId, TransportPlan>& transports,
                  std::optional<ModuloMapping>& answer) {
  if (edgeIndex == dfg.edges().size()) {
    try {
      ModuloMappingBuilder builder(dfg, ii);
      for (const auto& node : dfg.nodes())
        builder.place(node.id, placements.at(node.id).tile, placements.at(node.id).issueSlot);
      for (const auto& edge : dfg.edges()) {
        if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
          builder.setMemorySeparation(
              edge.id, target.memoryDependenceSeparation(
                           std::get<cgra::ir::MemoryEdgeInfo>(edge.info).dependence));
        } else {
          builder.setTransport(edge.id, transports.at(edge.id));
        }
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

  const auto& edge = dfg.edges()[edgeIndex];
  if (edge.kind() == cgra::ir::Edge::Kind::Memory)
    return realizeEdges(dfg, target, resources, reservations, ii, edgeIndex + 1, placements,
                        transports, answer);

  const auto& producer = placements.at(edge.src);
  const auto& consumer = placements.at(edge.dst);
  return routeCandidates(
      dfg, target, resources, reservations, ii, edge, producer, consumer,
      [&](const TransportPlan& plan, const std::vector<ResourceId>& routeResources) {
        const auto delta = reservations.tryReserve(std::span<const ResourceId>(routeResources),
                                                   {ReservationOwnerKind::Edge, edge.id});
        if (!delta)
          return false;
        transports.emplace(edge.id, plan);
        if (realizeEdges(dfg, target, resources, reservations, ii, edgeIndex + 1, placements,
                         transports, answer))
          return true;
        transports.erase(edge.id);
        reservations.undo(*delta);
        return false;
      });
}

bool searchPlacements(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                      ModuloResourceModel& resources, ResourceReservationTable& reservations,
                      std::uint32_t ii, std::size_t index,
                      std::map<cgra::target::TargetNodeId, NodePlacement>& placements,
                      std::optional<ModuloMapping>& answer) {
  if (index == dfg.nodes().size()) {
    std::map<cgra::target::TargetEdgeId, TransportPlan> transports;
    return realizeEdges(dfg, target, resources, reservations, ii, 0, placements, transports,
                        answer);
  }
  const auto& node = dfg.nodes()[index];
  auto tiles = target.compatibleTiles(node.operation);
  std::ranges::sort(tiles);
  for (const auto& [row, col] : tiles) {
    for (std::uint32_t slot = 0; slot < ii; ++slot) {
      std::vector<ResourceId> footprint;
      try {
        footprint = resources.operationFootprint(node, {row, col}, ModuloSlot(slot));
      } catch (const std::exception&) {
        continue;
      }
      const auto reservation = reservations.tryReserve(std::span<const ResourceId>(footprint),
                                                       {ReservationOwnerKind::Node, node.id});
      if (!reservation)
        continue;
      placements.emplace(node.id, NodePlacement{node.id, {row, col}, ModuloSlot(slot)});
      if (searchPlacements(dfg, target, resources, reservations, ii, index + 1, placements, answer))
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
  const auto tileCount = static_cast<std::uint64_t>(target.array().rows) * target.array().cols;
  if (ii == 0 || ii > options.maxII || dfg.nodes().size() > options.maxNodes ||
      dfg.edges().size() > options.maxEdges || tileCount > options.maxTiles) {
    result.status = ExactOracleStatus::UnsupportedOracleSize;
    return result;
  }
  if (!cgra::target::TargetDFGVerifier::verify(dfg, target).ok()) {
    result.status = ExactOracleStatus::InvalidInput;
    return result;
  }
  ModuloResourceModel resources(target, ii);
  ResourceReservationTable reservations(resources);
  std::map<cgra::target::TargetNodeId, NodePlacement> placements;
  if (searchPlacements(dfg, target, resources, reservations, ii, 0, placements, result.mapping))
    result.status = ExactOracleStatus::Feasible;
  else
    result.status = ExactOracleStatus::Infeasible;
  return result;
}

} // namespace cgra::mapping
