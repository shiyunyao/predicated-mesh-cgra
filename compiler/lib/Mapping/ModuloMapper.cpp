// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMapper.h"

#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace cgra::mapping {
namespace {

using Json = nlohmann::json;
using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

void addDiagnostic(ModuloMapperResult& result, ModuloMapperDiagnosticCode code, std::string message,
                   std::optional<std::uint32_t> ii = std::nullopt,
                   std::optional<NodeId> node = std::nullopt,
                   std::optional<EdgeId> edge = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), ii, node, edge});
}

enum class SearchOutcome {
  Success,
  Exhausted,
  BudgetExceeded,
  RouteBudgetExceeded,
  VerificationFailure,
  InternalError,
};

struct Candidate {
  TileCoord tile;
  ModuloSlot slot;
  std::uint64_t locality = 0;
};

struct CandidateDelta {
  NodeId node = 0;
  ReservationDelta nodeReservation;
  std::vector<EdgeId> edges;
  std::vector<ReservationDelta> edgeReservations;
};

class MappingSearchState {
public:
  MappingSearchState(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                     const ModuloMapperOptions& options, std::uint32_t ii,
                     ModuloMapperResult& result)
      : dfg_(dfg), target_(target), options_(options), ii_(ii), resources_(target, ii),
        reservations_(resources_), result_(result) {}

  SearchOutcome run() { return search(0); }

  std::optional<ModuloMapping> mapping() const noexcept { return mapping_; }

private:
  const cgra::target::TargetDFG& dfg_;
  const cgra::TargetModel& target_;
  const ModuloMapperOptions& options_;
  std::uint32_t ii_;
  ModuloResourceModel resources_;
  ResourceReservationTable reservations_;
  ModuloMapperResult& result_;
  std::map<NodeId, NodePlacement> placements_;
  std::map<EdgeId, MappedDependence> dependences_;
  std::optional<ModuloMapping> mapping_;
  bool verificationFailureReported_ = false;

  bool hasPlacement(NodeId id) const { return placements_.contains(id); }

  bool allNodesMapped() const { return placements_.size() == dfg_.nodes().size(); }

  std::uint64_t locality(NodeId node, TileCoord tile) const {
    std::uint64_t score = 0;
    for (const auto& edge : dfg_.edges()) {
      if (edge.src != node && edge.dst != node)
        continue;
      const auto other = edge.src == node ? edge.dst : edge.src;
      const auto found = placements_.find(other);
      if (found == placements_.end())
        continue;
      const auto& otherTile = found->second.tile;
      score += static_cast<std::uint64_t>(
          std::abs(static_cast<int>(tile.row) - static_cast<int>(otherTile.row)) +
          std::abs(static_cast<int>(tile.col) - static_cast<int>(otherTile.col)));
    }
    return score;
  }

  NodeId selectNode() const {
    const cgra::target::TargetNode* selected = nullptr;
    std::uint64_t selectedMapped = 0;
    std::uint64_t selectedDegree = 0;
    std::uint64_t selectedLoop = 0;
    for (const auto& node : dfg_.nodes()) {
      if (hasPlacement(node.id))
        continue;
      std::uint64_t mapped = 0;
      std::uint64_t degree = 0;
      std::uint64_t loop = 0;
      for (const auto& edge : dfg_.edges()) {
        if (edge.src != node.id && edge.dst != node.id)
          continue;
        ++degree;
        if (edge.distance != 0)
          ++loop;
        const auto other = edge.src == node.id ? edge.dst : edge.src;
        if (hasPlacement(other))
          ++mapped;
      }
      if (!selected ||
          std::tuple{mapped, degree, loop, std::numeric_limits<NodeId>::max() - node.id} >
              std::tuple{selectedMapped, selectedDegree, selectedLoop,
                         std::numeric_limits<NodeId>::max() - selected->id}) {
        selected = &node;
        selectedMapped = mapped;
        selectedDegree = degree;
        selectedLoop = loop;
      }
    }
    if (!selected)
      throw std::logic_error("no unmapped node remains before mapping completion");
    return selected->id;
  }

  std::vector<Candidate> candidates(NodeId node) const {
    const auto& targetNode = dfg_.node(node);
    std::vector<Candidate> values;
    for (const auto& [row, col] : target_.compatibleTiles(targetNode.operation)) {
      const TileCoord tile{row, col};
      for (std::uint32_t slot = 0; slot < ii_; ++slot)
        values.push_back({tile, ModuloSlot(slot), locality(node, tile)});
    }
    std::ranges::sort(values, [](const Candidate& lhs, const Candidate& rhs) {
      return std::tuple{lhs.locality, lhs.tile.row, lhs.tile.col, lhs.slot.value()} <
             std::tuple{rhs.locality, rhs.tile.row, rhs.tile.col, rhs.slot.value()};
    });
    return values;
  }

  std::vector<EdgeId> closedEdges(NodeId node) const {
    std::vector<EdgeId> edges;
    for (const auto& edge : dfg_.edges()) {
      if (edge.src != node && edge.dst != node)
        continue;
      if (hasPlacement(edge.src) && hasPlacement(edge.dst) && !dependences_.contains(edge.id))
        edges.push_back(edge.id);
    }
    std::ranges::sort(edges);
    return edges;
  }

  std::vector<ResourceId> routeResources(const TransportPlan& plan,
                                         const NodePlacement& producer) const {
    std::vector<ResourceId> resources;
    const ModuloTimeDomain time(ii_);
    for (const auto& action : plan.actions) {
      const auto* link = std::get_if<LinkStep>(&action);
      if (!link)
        continue;
      const auto slot = time.advance(producer.issueSlot, link->elapsedFromProducerIssue);
      const auto resource =
          resources_.linkResource(plan.domain, link->source, link->direction, slot);
      if (!resource)
        throw std::logic_error("route search returned a link outside the target topology");
      if (std::ranges::find(resources, *resource) != resources.end())
        throw std::logic_error("route search returned a self-conflicting route");
      resources.push_back(*resource);
    }
    return resources;
  }

  SearchOutcome realizeEdge(EdgeId edgeId, CandidateDelta& delta) {
    const auto& edge = dfg_.edge(edgeId);
    const auto& producer = placements_.at(edge.src);
    const auto& consumer = placements_.at(edge.dst);
    if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
      const auto separation = target_.memoryDependenceSeparation(
          std::get<cgra::ir::MemoryEdgeInfo>(edge.info).dependence);
      dependences_.emplace(edge.id,
                           MappedDependence{edge.id, edge.kind(), separation, std::nullopt});
      delta.edges.push_back(edge.id);
      return SearchOutcome::Success;
    }

    if (result_.stats.routeSearchCalls >= options_.budget.maxRouteSearchCalls) {
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_GLOBAL_BUDGET_EXCEEDED,
                    "maximum route-search call budget was exhausted", ii_, std::nullopt, edge.id);
      return SearchOutcome::BudgetExceeded;
    }
    ++result_.stats.routeSearchCalls;
    auto routeOptions = options_.routeOptions;
    routeOptions.budget = options_.budget.perRouteBudget;
    const auto route = ModuloRouteSearch::search(dfg_, target_, resources_, reservations_,
                                                 {edge.id, producer, consumer}, routeOptions);
    result_.stats.totalRouteStateExpansions += route.stats.stateExpansions;
    if (route.status == RouteSearchStatus::NoPath) {
      ++result_.stats.routeNoPaths;
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_ROUTE_NO_PATH, route.format(), ii_,
                    std::nullopt, edge.id);
      return SearchOutcome::Exhausted;
    }
    if (route.status == RouteSearchStatus::BudgetExceeded) {
      ++result_.stats.routeBudgetExceeded;
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_ROUTE_BUDGET_EXCEEDED, route.format(),
                    ii_, std::nullopt, edge.id);
      return SearchOutcome::RouteBudgetExceeded;
    }
    if (!route.ok()) {
      if (route.status == RouteSearchStatus::TargetContractError) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_TARGET_CONTRACT_ERROR,
                      route.format(), ii_, std::nullopt, edge.id);
        return SearchOutcome::InternalError;
      }
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR, route.format(), ii_,
                    std::nullopt, edge.id);
      return SearchOutcome::InternalError;
    }
    ++result_.stats.routeSuccesses;
    const auto routeIds = routeResources(*route.plan, producer);
    const auto reservation = reservations_.tryReserve(std::span<const ResourceId>(routeIds),
                                                      {ReservationOwnerKind::Edge, edge.id});
    if (!reservation) {
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR,
                    "route search returned a route that could not be reserved", ii_, std::nullopt,
                    edge.id);
      return SearchOutcome::InternalError;
    }
    delta.edgeReservations.push_back(*reservation);
    delta.edges.push_back(edge.id);
    dependences_.emplace(
        edge.id,
        MappedDependence{edge.id, edge.kind(), route.plan->requiredSeparationCycles, route.plan});
    return SearchOutcome::Success;
  }

  SearchOutcome tryCandidate(NodeId node, const Candidate& candidate, CandidateDelta& delta) {
    const auto& targetNode = dfg_.node(node);
    std::vector<ResourceId> footprint;
    try {
      footprint = resources_.operationFootprint(targetNode, candidate.tile, candidate.slot);
    } catch (const std::exception&) {
      ++result_.stats.rejectedPlacements;
      return SearchOutcome::Exhausted;
    }
    const auto nodeReservation = reservations_.tryReserve(std::span<const ResourceId>(footprint),
                                                          {ReservationOwnerKind::Node, node});
    if (!nodeReservation) {
      ++result_.stats.rejectedPlacements;
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_NODE_RESOURCE_CONFLICT,
                    "candidate operation footprint conflicts with an existing reservation", ii_,
                    node);
      return SearchOutcome::Exhausted;
    }
    delta.node = node;
    delta.nodeReservation = *nodeReservation;
    placements_.emplace(node, NodePlacement{node, candidate.tile, candidate.slot});
    ++result_.stats.successfulPlacements;

    for (const auto edge : closedEdges(node)) {
      const auto outcome = realizeEdge(edge, delta);
      if (outcome != SearchOutcome::Success)
        return outcome;
    }
    return SearchOutcome::Success;
  }

  void rollback(CandidateDelta& delta) {
    for (const auto edge : delta.edges)
      dependences_.erase(edge);
    for (auto it = delta.edgeReservations.rbegin(); it != delta.edgeReservations.rend(); ++it)
      reservations_.undo(*it);
    if (!delta.nodeReservation.resources.empty())
      reservations_.undo(delta.nodeReservation);
    placements_.erase(delta.node);
  }

  std::optional<ModuloMapping> materialize() {
    try {
      ModuloMappingBuilder builder(dfg_, ii_);
      for (const auto& [id, placement] : placements_)
        builder.place(id, placement.tile, placement.issueSlot);
      for (const auto& [id, dependence] : dependences_) {
        if (dependence.transport)
          builder.setTransport(id, *dependence.transport);
        else
          builder.setMemorySeparation(id, dependence.requiredSeparationCycles);
      }
      return builder.finish();
    } catch (const std::exception& error) {
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR,
                    std::string("failed to materialize mapping: ") + error.what(), ii_);
      return std::nullopt;
    }
  }

  SearchOutcome complete() {
    mapping_ = materialize();
    if (!mapping_)
      return SearchOutcome::InternalError;
    const auto report = ModuloMappingVerifier::verify(dfg_, target_, *mapping_);
    if (!report.ok()) {
      if (!verificationFailureReported_) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_FINAL_VERIFICATION_FAILED,
                      report.format(), ii_);
        verificationFailureReported_ = true;
      }
      return SearchOutcome::VerificationFailure;
    }
    return SearchOutcome::Success;
  }

  SearchOutcome search(std::uint64_t depth) {
    result_.stats.maxSearchDepth = std::max(result_.stats.maxSearchDepth, depth);
    if (allNodesMapped())
      return complete();

    const auto node = selectNode();
    const auto nodeCandidates = candidates(node);
    if (nodeCandidates.empty()) {
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_NO_COMPATIBLE_CANDIDATE,
                    "unmapped node has no compatible tile candidates", ii_, node);
      return SearchOutcome::Exhausted;
    }
    for (const auto& candidate : nodeCandidates) {
      if (result_.stats.nodeCandidateAttempts >= options_.budget.maxNodeCandidateAttempts) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_GLOBAL_BUDGET_EXCEEDED,
                      "maximum node-candidate budget was exhausted", ii_, node);
        return SearchOutcome::BudgetExceeded;
      }
      ++result_.stats.nodeCandidateAttempts;
      CandidateDelta delta;
      const auto candidateOutcome = tryCandidate(node, candidate, delta);
      if (candidateOutcome == SearchOutcome::Exhausted) {
        if (delta.nodeReservation.resources.empty())
          continue;
        rollback(delta);
        continue;
      } else if (candidateOutcome != SearchOutcome::Success) {
        rollback(delta);
        return candidateOutcome;
      }

      const auto childOutcome = search(depth + 1);
      if (childOutcome == SearchOutcome::Success)
        return childOutcome;
      rollback(delta);
      if (childOutcome == SearchOutcome::BudgetExceeded ||
          childOutcome == SearchOutcome::RouteBudgetExceeded ||
          childOutcome == SearchOutcome::VerificationFailure ||
          childOutcome == SearchOutcome::InternalError)
        return childOutcome;
      if (result_.stats.backtracks >= options_.budget.maxBacktracks) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_GLOBAL_BUDGET_EXCEEDED,
                      "maximum backtrack budget was exhausted", ii_, node);
        return SearchOutcome::BudgetExceeded;
      }
      ++result_.stats.backtracks;
    }
    return SearchOutcome::Exhausted;
  }
};

} // namespace

std::string_view toString(ModuloMapperStatus status) noexcept {
  switch (status) {
  case ModuloMapperStatus::Success:
    return "success";
  case ModuloMapperStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case ModuloMapperStatus::MIIAnalysisFailure:
    return "mii_analysis_failure";
  case ModuloMapperStatus::NoMappingWithinIILimit:
    return "no_mapping_within_ii_limit";
  case ModuloMapperStatus::BudgetExceeded:
    return "budget_exceeded";
  case ModuloMapperStatus::RouteBudgetExceeded:
    return "route_budget_exceeded";
  case ModuloMapperStatus::VerificationFailure:
    return "verification_failure";
  case ModuloMapperStatus::TargetContractError:
    return "target_contract_error";
  case ModuloMapperStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(ModuloMapperDiagnosticCode code) noexcept {
  switch (code) {
  case ModuloMapperDiagnosticCode::MAP_INVALID_TARGET_DFG:
    return "MAP_INVALID_TARGET_DFG";
  case ModuloMapperDiagnosticCode::MAP_MII_ANALYSIS_FAILED:
    return "MAP_MII_ANALYSIS_FAILED";
  case ModuloMapperDiagnosticCode::MAP_NO_COMPATIBLE_CANDIDATE:
    return "MAP_NO_COMPATIBLE_CANDIDATE";
  case ModuloMapperDiagnosticCode::MAP_NODE_RESOURCE_CONFLICT:
    return "MAP_NODE_RESOURCE_CONFLICT";
  case ModuloMapperDiagnosticCode::MAP_ROUTE_NO_PATH:
    return "MAP_ROUTE_NO_PATH";
  case ModuloMapperDiagnosticCode::MAP_ROUTE_BUDGET_EXCEEDED:
    return "MAP_ROUTE_BUDGET_EXCEEDED";
  case ModuloMapperDiagnosticCode::MAP_GLOBAL_BUDGET_EXCEEDED:
    return "MAP_GLOBAL_BUDGET_EXCEEDED";
  case ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT:
    return "MAP_NO_MAPPING_WITHIN_II_LIMIT";
  case ModuloMapperDiagnosticCode::MAP_FINAL_VERIFICATION_FAILED:
    return "MAP_FINAL_VERIFICATION_FAILED";
  case ModuloMapperDiagnosticCode::MAP_TARGET_CONTRACT_ERROR:
    return "MAP_TARGET_CONTRACT_ERROR";
  case ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR:
    return "MAP_INTERNAL_ERROR";
  }
  return "MAP_INTERNAL_ERROR";
}

std::string ModuloMapperResult::format() const {
  std::ostringstream output;
  output << toString(status) << " modulo mapper";
  output << "\nstarting MII: " << stats.startingMII << "\nfinal II: " << stats.finalII
         << "\nII attempts: " << stats.iiAttempts
         << "\nnode candidate attempts: " << stats.nodeCandidateAttempts
         << "\nroute searches: " << stats.routeSearchCalls << "\nbacktracks: " << stats.backtracks;
  for (const auto& diagnostic : diagnostics)
    output << '\n' << toString(diagnostic.code) << ": " << diagnostic.message;
  return output.str();
}

std::string ModuloMapperResult::toJson() const {
  Json root = {{"schema", "cgra.modulo_mapper.result.v1"},
               {"status", toString(status)},
               {"stats",
                {{"starting_mii", stats.startingMII},
                 {"final_ii", stats.finalII},
                 {"ii_attempts", stats.iiAttempts},
                 {"node_candidate_attempts", stats.nodeCandidateAttempts},
                 {"successful_placements", stats.successfulPlacements},
                 {"rejected_placements", stats.rejectedPlacements},
                 {"route_search_calls", stats.routeSearchCalls},
                 {"route_successes", stats.routeSuccesses},
                 {"route_no_paths", stats.routeNoPaths},
                 {"route_budget_exceeded", stats.routeBudgetExceeded},
                 {"backtracks", stats.backtracks},
                 {"max_search_depth", stats.maxSearchDepth},
                 {"total_route_state_expansions", stats.totalRouteStateExpansions}}},
               {"diagnostics", Json::array()}};
  if (mapping)
    root["mapping"] = {{"target", mapping->targetName()}, {"ii", mapping->ii()}};
  for (const auto& diagnostic : diagnostics) {
    Json value = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.ii)
      value["ii"] = *diagnostic.ii;
    if (diagnostic.node)
      value["node"] = *diagnostic.node;
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    root["diagnostics"].push_back(std::move(value));
  }
  return root.dump(2) + '\n';
}

ModuloMapperResult ModuloMapper::map(const cgra::target::TargetDFG& dfg,
                                     const cgra::TargetModel& target,
                                     const ModuloMapperOptions& options) {
  ModuloMapperResult result;
  const auto mii = cgra::analysis::MIIAnalyzer::analyze(dfg, target);
  if (!mii.ok()) {
    switch (mii.status) {
    case cgra::analysis::MIIStatus::InvalidTargetDFG:
      result.status = ModuloMapperStatus::InvalidTargetDFG;
      addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_INVALID_TARGET_DFG, mii.format());
      break;
    case cgra::analysis::MIIStatus::TargetContractError:
      result.status = ModuloMapperStatus::TargetContractError;
      addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_TARGET_CONTRACT_ERROR, mii.format());
      break;
    default:
      result.status = ModuloMapperStatus::MIIAnalysisFailure;
      addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_MII_ANALYSIS_FAILED, mii.format());
      break;
    }
    return result;
  }
  if (mii.mii == 0 || mii.mii == std::numeric_limits<std::uint32_t>::max()) {
    result.status = ModuloMapperStatus::MIIAnalysisFailure;
    addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_MII_ANALYSIS_FAILED,
                  "MII analyzer returned an invalid starting II");
    return result;
  }
  result.stats.startingMII = mii.mii;
  const auto maxII = options.maxII == 0 ? mii.mii : options.maxII;
  if (maxII < mii.mii) {
    result.status = ModuloMapperStatus::NoMappingWithinIILimit;
    addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
                  "maxII is below the analyzer lower bound", mii.mii);
    return result;
  }

  for (std::uint64_t ii = mii.mii; ii <= maxII; ++ii) {
    ++result.stats.iiAttempts;
    const auto currentII = static_cast<std::uint32_t>(ii);
    MappingSearchState state(dfg, target, options, currentII, result);
    const auto outcome = state.run();
    result.stats.finalII = currentII;
    if (outcome == SearchOutcome::Success) {
      result.status = ModuloMapperStatus::Success;
      result.mapping = state.mapping();
      return result;
    }
    if (outcome == SearchOutcome::BudgetExceeded) {
      result.status = ModuloMapperStatus::BudgetExceeded;
      return result;
    }
    if (outcome == SearchOutcome::RouteBudgetExceeded) {
      result.status = ModuloMapperStatus::RouteBudgetExceeded;
      return result;
    }
    if (outcome == SearchOutcome::VerificationFailure) {
      result.status = ModuloMapperStatus::VerificationFailure;
      result.mapping.reset();
      return result;
    }
    if (outcome == SearchOutcome::InternalError) {
      result.status = ModuloMapperStatus::InternalError;
      result.mapping.reset();
      return result;
    }
  }
  result.status = ModuloMapperStatus::NoMappingWithinIILimit;
  addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
                "all II values through maxII were fully exhausted");
  return result;
}

} // namespace cgra::mapping
