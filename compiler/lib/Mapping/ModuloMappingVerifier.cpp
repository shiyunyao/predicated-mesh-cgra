// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingVerifier.h"

#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <type_traits>
#include <unordered_set>

namespace cgra::mapping {
namespace {

using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

struct Availability {
  TileCoord tile;
  std::uint64_t elapsed = 0;
};

void add(ModuloMappingVerificationReport& report, MappingDiagnosticCode code, std::string message,
         std::optional<NodeId> node = std::nullopt, std::optional<EdgeId> edge = std::nullopt,
         std::optional<ResourceId> resource = std::nullopt,
         std::optional<TileCoord> tile = std::nullopt,
         std::optional<std::uint32_t> slot = std::nullopt,
         std::optional<std::uint32_t> action = std::nullopt) {
  report.add({MappingDiagnosticSeverity::Error, code, std::move(message), node, edge, resource,
              tile, slot, action});
}

NetworkDomain expectedDomain(cgra::ir::Edge::Kind kind) {
  return kind == cgra::ir::Edge::Kind::Predicate ? NetworkDomain::Predicate : NetworkDomain::Data;
}

MappingDiagnosticCode conflictCode(const ModuloResource& resource) {
  switch (kindOf(resource)) {
  case ResourceKind::FU:
    return MappingDiagnosticCode::MMAP_FU_RESOURCE_CONFLICT;
  case ResourceKind::LSU:
    return MappingDiagnosticCode::MMAP_LSU_RESOURCE_CONFLICT;
  case ResourceKind::DataLink:
    return MappingDiagnosticCode::MMAP_DATA_LINK_CONFLICT;
  case ResourceKind::PredicateLink:
    return MappingDiagnosticCode::MMAP_PRED_LINK_CONFLICT;
  }
  return MappingDiagnosticCode::MMAP_INTERNAL_ERROR;
}

TileCoord resourceTile(const ModuloResource& resource) {
  return std::visit(
      [](const auto& value) -> TileCoord {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LinkResource>)
          return value.source;
        else
          return value.tile;
      },
      resource);
}

ModuloSlot resourceSlot(const ModuloResource& resource) {
  return std::visit([](const auto& value) { return value.slot; }, resource);
}

bool hasUniquePlacement(const std::map<NodeId, std::vector<const NodePlacement*>>& placements,
                        NodeId id) {
  const auto it = placements.find(id);
  return it != placements.end() && it->second.size() == 1;
}

bool hasUniqueDependence(const std::map<EdgeId, std::vector<const MappedDependence*>>& dependences,
                         EdgeId id) {
  const auto it = dependences.find(id);
  return it != dependences.end() && it->second.size() == 1;
}

void verifyValueTransport(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                          const cgra::target::TargetEdge& edge, const MappedDependence& dependence,
                          const std::map<NodeId, const NodePlacement*>& placements,
                          ResourceReservationTable& reservations, ModuloResourceModel& resources,
                          ModuloMappingVerificationReport& report) {
  if (!dependence.transport) {
    add(report, MappingDiagnosticCode::MMAP_TRANSPORT_MISSING_FOR_VALUE_EDGE,
        "value dependence has no transport plan", std::nullopt, edge.id);
    return;
  }
  const auto& plan = *dependence.transport;
  if (plan.edge != edge.id) {
    add(report, MappingDiagnosticCode::MMAP_TRANSPORT_BAD_START,
        "transport plan edge ID disagrees with target edge", std::nullopt, edge.id);
    return;
  }
  const auto domain = expectedDomain(edge.kind());
  if (plan.domain != domain) {
    add(report, MappingDiagnosticCode::MMAP_TRANSPORT_DOMAIN_MISMATCH,
        "transport network domain disagrees with target edge kind", std::nullopt, edge.id);
    return;
  }
  const auto srcPlacementIt = placements.find(edge.src);
  const auto dstPlacementIt = placements.find(edge.dst);
  if (srcPlacementIt == placements.end() || dstPlacementIt == placements.end())
    return;
  const auto& srcPlacement = *srcPlacementIt->second;
  const auto& dstPlacement = *dstPlacementIt->second;
  const auto& producer = dfg.node(edge.src);

  if (!producer.producerOutputReadyOffset) {
    add(report, MappingDiagnosticCode::MMAP_TARGET_TIMING_MISSING,
        "value-producing target operation has no producer output-ready offset", edge.src, edge.id);
    return;
  }
  const auto& network =
      plan.domain == NetworkDomain::Predicate ? target.predicateNetwork() : target.dataNetwork();
  const auto hopLatency = network.hopLatency;
  if (hopLatency == 0) {
    add(report, MappingDiagnosticCode::MMAP_TARGET_TIMING_MISSING,
        "target network hop latency must be positive", std::nullopt, edge.id);
    return;
  }

  Availability availability{srcPlacement.tile, *producer.producerOutputReadyOffset};
  const auto producerReady = availability.elapsed;
  std::unordered_set<ResourceId> routeResources;
  std::vector<ResourceId> routeResourceList;
  bool routeValid = true;
  for (std::size_t index = 0; index < plan.actions.size(); ++index) {
    const auto& action = plan.actions[index];
    const auto actionDomain = std::visit([](const auto& value) { return value.domain; }, action);
    if (actionDomain != plan.domain) {
      add(report, MappingDiagnosticCode::MMAP_TRANSPORT_DOMAIN_MISMATCH,
          "transport action domain disagrees with plan", std::nullopt, edge.id, std::nullopt,
          std::nullopt, std::nullopt, static_cast<std::uint32_t>(index));
      routeValid = false;
      continue;
    }
    if (const auto* link = std::get_if<LinkStep>(&action)) {
      if (index == 0 && link->elapsedFromProducerIssue < producerReady) {
        add(report, MappingDiagnosticCode::MMAP_LINK_TIME_BEFORE_VALUE_READY,
            "first link launch precedes producer output readiness", std::nullopt, edge.id,
            std::nullopt, link->source, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (link->elapsedFromProducerIssue < availability.elapsed) {
        add(report, MappingDiagnosticCode::MMAP_LINK_TIME_REGRESSION,
            "link launch occurs before the current availability time", std::nullopt, edge.id,
            std::nullopt, link->source, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (link->elapsedFromProducerIssue > availability.elapsed) {
        add(report, MappingDiagnosticCode::MMAP_LINK_TIME_GAP_WITHOUT_STORAGE,
            "link launch has an implicit wait without VirtualHold", std::nullopt, edge.id,
            std::nullopt, link->source, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (link->source != availability.tile) {
        add(report, MappingDiagnosticCode::MMAP_TRANSPORT_DISCONTINUITY,
            "link source does not match the current available tile", std::nullopt, edge.id,
            std::nullopt, link->source, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      const auto nextTile = neighbor(link->source, link->direction, target);
      const auto slot = ModuloTimeDomain(resources.ii())
                            .advance(srcPlacement.issueSlot, link->elapsedFromProducerIssue);
      const auto resource =
          resources.linkResource(plan.domain, link->source, link->direction, slot);
      if (!nextTile || !resource) {
        add(report, MappingDiagnosticCode::MMAP_LINK_INVALID_TOPOLOGY,
            "link does not exist in the target mesh", std::nullopt, edge.id, resource, link->source,
            slot.value(), static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (!routeResources.insert(*resource).second) {
        add(report, MappingDiagnosticCode::MMAP_ROUTE_SELF_RESOURCE_CONFLICT,
            "one transport route reuses a modulo link resource", std::nullopt, edge.id, resource,
            link->source, slot.value(), static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      routeResourceList.push_back(*resource);
      if (!reservations.isFree(*resource)) {
        const auto owner = reservations.owner(*resource);
        std::ostringstream message;
        message << "link resource conflicts with edge ";
        if (owner && owner->kind == ReservationOwnerKind::Edge)
          message << owner->id;
        else
          message << "a node";
        add(report, conflictCode(resources.resource(*resource)), message.str(), std::nullopt,
            edge.id, resource, link->source, slot.value(), static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      availability.tile = *nextTile;
      availability.elapsed =
          static_cast<std::uint64_t>(link->elapsedFromProducerIssue) + hopLatency;
    } else {
      const auto& hold = std::get<VirtualHold>(action);
      if (hold.tile != availability.tile) {
        add(report, MappingDiagnosticCode::MMAP_HOLD_WRONG_TILE,
            "VirtualHold tile does not match current availability", std::nullopt, edge.id,
            std::nullopt, hold.tile, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (hold.captureElapsed < availability.elapsed) {
        add(report, MappingDiagnosticCode::MMAP_HOLD_BEFORE_VALUE_READY,
            "VirtualHold captures before the value is available", std::nullopt, edge.id,
            std::nullopt, hold.tile, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (hold.captureElapsed > availability.elapsed) {
        add(report, MappingDiagnosticCode::MMAP_HOLD_DISCONTINUITY,
            "VirtualHold capture has an implicit wait", std::nullopt, edge.id, std::nullopt,
            hold.tile, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      if (hold.releaseElapsed <= hold.captureElapsed) {
        add(report, MappingDiagnosticCode::MMAP_HOLD_INVALID_INTERVAL,
            "VirtualHold release must be after capture", std::nullopt, edge.id, std::nullopt,
            hold.tile, std::nullopt, static_cast<std::uint32_t>(index));
        routeValid = false;
        continue;
      }
      availability.elapsed = hold.releaseElapsed;
    }
  }
  if (!routeValid)
    return;
  if (plan.actions.empty() && srcPlacement.tile == dstPlacement.tile) {
    add(report, MappingDiagnosticCode::MMAP_TRANSPORT_BAD_END,
        "same-tile value dependence requires explicit VirtualHold", std::nullopt, edge.id);
    return;
  }
  if (availability.tile != dstPlacement.tile) {
    add(report, MappingDiagnosticCode::MMAP_TRANSPORT_BAD_END,
        "transport finishes on a tile different from the consumer", edge.dst, edge.id, std::nullopt,
        availability.tile);
    return;
  }
  if (availability.elapsed != plan.requiredSeparationCycles ||
      dependence.requiredSeparationCycles != plan.requiredSeparationCycles) {
    add(report, MappingDiagnosticCode::MMAP_REQUIRED_SEPARATION_MISMATCH,
        "cached required separation does not match reconstructed transport timing", std::nullopt,
        edge.id);
    return;
  }
  if (!routeResourceList.empty() &&
      !reservations.reserve(std::span<const ResourceId>(routeResourceList),
                            {ReservationOwnerKind::Edge, edge.id})) {
    add(report, MappingDiagnosticCode::MMAP_INTERNAL_ERROR,
        "fresh reservation table rejected a previously free transport route", std::nullopt,
        edge.id);
  }
}

} // namespace

ModuloMappingVerificationReport ModuloMappingVerifier::verify(const cgra::target::TargetDFG& dfg,
                                                              const cgra::TargetModel& target,
                                                              const ModuloMapping& mapping) {
  ModuloMappingVerificationReport report;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    add(report, MappingDiagnosticCode::MMAP_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return report;
  }
  if (dfg.targetName() != target.name()) {
    add(report, MappingDiagnosticCode::MMAP_INVALID_TARGET_DFG,
        "TargetDFG target name does not match selected TargetModel");
    return report;
  }
  if (mapping.ii() == 0) {
    add(report, MappingDiagnosticCode::MMAP_INVALID_II, "mapping II must be at least one");
    return report;
  }
  if (mapping.targetName() != target.name()) {
    add(report, MappingDiagnosticCode::MMAP_TARGET_NAME_MISMATCH,
        "mapping target name does not match selected TargetModel");
    return report;
  }

  ModuloResourceModel resources(target, mapping.ii());
  ResourceReservationTable reservations(resources);

  std::map<NodeId, std::vector<const NodePlacement*>> placementEntries;
  for (const auto& placement : mapping.placements())
    placementEntries[placement.node].push_back(&placement);
  std::map<EdgeId, std::vector<const MappedDependence*>> dependenceEntries;
  for (const auto& dependence : mapping.dependences())
    dependenceEntries[dependence.edge].push_back(&dependence);

  std::map<NodeId, const cgra::target::TargetNode*> nodes;
  for (const auto& node : dfg.nodes())
    nodes.emplace(node.id, &node);
  std::map<EdgeId, const cgra::target::TargetEdge*> edges;
  for (const auto& edge : dfg.edges())
    edges.emplace(edge.id, &edge);

  for (const auto& [id, entries] : placementEntries) {
    if (!nodes.contains(id))
      add(report, MappingDiagnosticCode::MMAP_UNKNOWN_NODE,
          "mapping contains placement for unknown TargetDFG node", id);
    if (entries.size() > 1)
      add(report, MappingDiagnosticCode::MMAP_NODE_DUPLICATE_PLACEMENT,
          "mapping contains duplicate node placements", id);
  }
  for (const auto& [id, entries] : dependenceEntries) {
    if (!edges.contains(id))
      add(report, MappingDiagnosticCode::MMAP_UNKNOWN_EDGE,
          "mapping contains realization for unknown TargetDFG edge", std::nullopt, id);
    if (entries.size() > 1)
      add(report, MappingDiagnosticCode::MMAP_EDGE_DUPLICATE_REALIZATION,
          "mapping contains duplicate edge realizations", std::nullopt, id);
  }
  for (const auto& [id, node] : nodes)
    if (!hasUniquePlacement(placementEntries, id))
      add(report, MappingDiagnosticCode::MMAP_NODE_MISSING_PLACEMENT,
          "TargetDFG node has no unique placement", id);
  for (const auto& [id, edge] : edges)
    if (!hasUniqueDependence(dependenceEntries, id))
      add(report, MappingDiagnosticCode::MMAP_EDGE_MISSING_REALIZATION,
          "TargetDFG edge has no unique mapping realization", std::nullopt, id);

  std::map<NodeId, const NodePlacement*> placements;
  for (const auto& [id, entries] : placementEntries)
    if (nodes.contains(id) && entries.size() == 1)
      placements.emplace(id, entries.front());

  for (const auto& [id, node] : nodes) {
    const auto placementIt = placements.find(id);
    if (placementIt == placements.end())
      continue;
    const auto& placement = *placementIt->second;
    if (placement.tile.row >= target.array().rows || placement.tile.col >= target.array().cols) {
      add(report, MappingDiagnosticCode::MMAP_TILE_OUT_OF_RANGE,
          "node placement tile is outside target array", id, std::nullopt, std::nullopt,
          placement.tile, placement.issueSlot.value());
      continue;
    }
    if (placement.issueSlot.value() >= mapping.ii()) {
      add(report, MappingDiagnosticCode::MMAP_SLOT_OUT_OF_RANGE,
          "node placement slot is outside [0, II)", id, std::nullopt, std::nullopt, placement.tile,
          placement.issueSlot.value());
      continue;
    }
    if (!resources.supportsOperation(placement.tile, *node)) {
      const auto* operation = target.findOperation(node->operation);
      add(report,
          operation && operation->executionClass == cgra::TargetExecutionClass::LSU
              ? MappingDiagnosticCode::MMAP_OPERATION_UNSUPPORTED_ON_TILE
              : MappingDiagnosticCode::MMAP_EXECUTION_RESOURCE_MISSING,
          "target operation cannot execute on the selected tile", id, std::nullopt, std::nullopt,
          placement.tile, placement.issueSlot.value());
      continue;
    }
    try {
      const auto footprint =
          resources.operationFootprint(*node, placement.tile, placement.issueSlot);
      if (!reservations.canReserve(footprint)) {
        for (const auto resource : footprint) {
          const auto owner = reservations.owner(resource);
          if (!owner)
            continue;
          const auto& semantic = resources.resource(resource);
          std::ostringstream message;
          message << "node resource conflicts with mapped node " << owner->id;
          add(report, conflictCode(semantic), message.str(), id, std::nullopt, resource,
              resourceTile(semantic), resourceSlot(semantic).value());
          break;
        }
      } else if (!reservations.reserve(footprint, {ReservationOwnerKind::Node, id})) {
        add(report, MappingDiagnosticCode::MMAP_INTERNAL_ERROR,
            "fresh reservation table rejected a previously free node footprint", id);
      }
    } catch (const std::invalid_argument& error) {
      const auto* operation = target.findOperation(node->operation);
      const auto selfOverlap = operation && operation->issueOccupancy > mapping.ii();
      add(report,
          selfOverlap ? MappingDiagnosticCode::MMAP_OPERATION_SELF_OVERLAP
                      : MappingDiagnosticCode::MMAP_EXECUTION_RESOURCE_MISSING,
          error.what(), id, std::nullopt, std::nullopt, placement.tile,
          placement.issueSlot.value());
    }
  }

  for (const auto& [id, edge] : edges) {
    const auto dependenceIt = dependenceEntries.find(id);
    if (dependenceIt == dependenceEntries.end() || dependenceIt->second.size() != 1)
      continue;
    const auto& dependence = *dependenceIt->second.front();
    if (dependence.kind != edge->kind()) {
      add(report, MappingDiagnosticCode::MMAP_TRANSPORT_DOMAIN_MISMATCH,
          "mapping dependence kind disagrees with TargetDFG edge", std::nullopt, id);
      continue;
    }
    if (edge->kind() == cgra::ir::Edge::Kind::Memory) {
      if (dependence.transport) {
        add(report, MappingDiagnosticCode::MMAP_MEMORY_TRANSPORT_PRESENT,
            "memory dependence must not contain a transport plan", std::nullopt, id);
        continue;
      }
      const auto expected = target.memoryDependenceSeparation(
          std::get<cgra::ir::MemoryEdgeInfo>(edge->info).dependence);
      if (dependence.requiredSeparationCycles < expected)
        add(report, MappingDiagnosticCode::MMAP_MEMORY_SEPARATION_TOO_SMALL,
            "memory dependence separation is below TargetModel minimum", std::nullopt, id);
      else if (dependence.requiredSeparationCycles != expected)
        add(report, MappingDiagnosticCode::MMAP_MEMORY_SEPARATION_MISMATCH,
            "memory dependence separation differs from TargetModel minimum", std::nullopt, id);
    } else if (hasUniquePlacement(placementEntries, edge->src) &&
               hasUniquePlacement(placementEntries, edge->dst)) {
      verifyValueTransport(dfg, target, *edge, dependence, placements, reservations, resources,
                           report);
    }
  }
  return report;
}

} // namespace cgra::mapping
