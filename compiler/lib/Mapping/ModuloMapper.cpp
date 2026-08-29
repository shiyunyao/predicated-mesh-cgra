// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMapper.h"

#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/Mapping/ConstructiveModuloMapper.h"
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
  constexpr std::size_t MaxDiagnosticsPerCode = 16;
  if (static_cast<std::size_t>(
          std::ranges::count_if(result.diagnostics, [code](const auto& diagnostic) {
            return diagnostic.code == code;
          })) >= MaxDiagnosticsPerCode) {
    ++result.suppressedDiagnostics;
    return;
  }
  result.diagnostics.push_back({code, std::move(message), ii, node, edge});
}

enum class SearchOutcome {
  Success,
  Exhausted,
  PerIIBudgetExceeded,
  BudgetExceeded,
  RouteBudgetExceeded,
  VerificationFailure,
  InternalError,
  PostMappingAbort,
};

struct SearchLimits {
  std::uint64_t nodeCandidateAttempts = 0;
  std::uint64_t backtracks = 0;
  std::uint64_t routeSearchCalls = 0;
};

struct Candidate {
  TileCoord tile;
  ModuloSlot slot;
  std::uint64_t stageWraps = 0;
  std::uint64_t locality = 0;
  std::uint64_t slotAffinity = 0;
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
                     const ModuloMapperOptions& options, SearchLimits limits, std::uint32_t ii,
                     ModuloMapperResult& result)
      : dfg_(dfg), target_(target), options_(options), ii_(ii), resources_(target, ii),
        reservations_(resources_), result_(result), limits_(limits) {}

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
  SearchLimits limits_;
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
    std::uint64_t selectedUnmappedInputs = 0;
    std::uint64_t selectedMapped = 0;
    std::uint64_t selectedDegree = 0;
    std::uint64_t selectedLoop = 0;
    for (const auto& node : dfg_.nodes()) {
      if (hasPlacement(node.id))
        continue;
      std::uint64_t mapped = 0;
      std::uint64_t degree = 0;
      std::uint64_t loop = 0;
      std::uint64_t unmappedInputs = 0;
      for (const auto& edge : dfg_.edges()) {
        if (edge.src != node.id && edge.dst != node.id)
          continue;
        ++degree;
        if (edge.distance != 0)
          ++loop;
        const auto other = edge.src == node.id ? edge.dst : edge.src;
        if (hasPlacement(other))
          ++mapped;
        if (edge.dst == node.id && edge.src != node.id && edge.distance == 0 &&
            edge.kind() != cgra::ir::Edge::Kind::Memory && !hasPlacement(edge.src))
          ++unmappedInputs;
      }
      if (!selected ||
          std::tuple{std::numeric_limits<std::uint64_t>::max() - unmappedInputs, mapped, degree,
                     loop, std::numeric_limits<NodeId>::max() - node.id} >
              std::tuple{std::numeric_limits<std::uint64_t>::max() - selectedUnmappedInputs,
                         selectedMapped, selectedDegree, selectedLoop,
                         std::numeric_limits<NodeId>::max() - selected->id}) {
        selected = &node;
        selectedUnmappedInputs = unmappedInputs;
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
      for (std::uint32_t slot = 0; slot < ii_; ++slot) {
        std::uint64_t affinity = 0;
        std::uint64_t stageWraps = 0;
        for (const auto& edge : dfg_.edges()) {
          if (edge.src != node && edge.dst != node)
            continue;
          const auto other = edge.src == node ? edge.dst : edge.src;
          const auto found = placements_.find(other);
          if (edge.kind() == cgra::ir::Edge::Kind::Memory)
            continue;
          const auto& sourceNode = dfg_.node(edge.src);
          const auto ready = sourceNode.producerOutputReadyOffset.value_or(0U) + 1U;
          if (found == placements_.end()) {
            if (edge.distance == 0 || edge.src == edge.dst)
              continue;
            const auto preferred = edge.src == node ? (ii_ - ready % ii_) % ii_ : 0U;
            if (slot != preferred)
              ++affinity;
            continue;
          }
          if (edge.distance == 0) {
            const auto separation =
                edge.dst == node
                    ? static_cast<std::int64_t>(slot) - found->second.issueSlot.value()
                    : static_cast<std::int64_t>(found->second.issueSlot.value()) - slot;
            if (separation < static_cast<std::int64_t>(ready))
              ++stageWraps;
          }
          const auto preferred = edge.dst == node
                                     ? (found->second.issueSlot.value() + ready) % ii_
                                     : (found->second.issueSlot.value() + ii_ - ready % ii_) % ii_;
          if (slot != preferred)
            ++affinity;
        }
        values.push_back({tile, ModuloSlot(slot), stageWraps, locality(node, tile), affinity});
      }
    }
    std::ranges::sort(values, [](const Candidate& lhs, const Candidate& rhs) {
      return std::tuple{lhs.stageWraps, lhs.locality, lhs.slotAffinity,
                        lhs.tile.row,   lhs.tile.col, lhs.slot.value()} <
             std::tuple{rhs.stageWraps, rhs.locality, rhs.slotAffinity,
                        rhs.tile.row,   rhs.tile.col, rhs.slot.value()};
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

    if (result_.stats.routeSearchCalls >= limits_.routeSearchCalls) {
      addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_II_BUDGET_SHARE_EXHAUSTED,
                    "per-II route-search budget share was exhausted", ii_, std::nullopt, edge.id);
      return SearchOutcome::PerIIBudgetExceeded;
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
    ++result_.stats.completedModuloMappings;
    if (options_.completeMappingChecker) {
      const auto check = options_.completeMappingChecker(dfg_, target_, *mapping_);
      if (check.decision == CompleteMappingDecision::Reject) {
        ++result_.stats.postMappingRejected;
        if (check.reasonCode.starts_with("stage"))
          ++result_.stats.stageRejected;
        if (check.reasonCode.starts_with("rf"))
          ++result_.stats.rfRejected;
        if (check.reasonCode.starts_with("rf"))
          ++result_.stats.rfRejectedByII[ii_];
        if (check.reasonCode.starts_with("rf"))
          ++result_.stats.rfRejectedByReason[check.reasonCode];
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_POST_MAPPING_REJECTED,
                      check.reasonCode + (check.message.empty() ? "" : ": " + check.message), ii_);
        mapping_.reset();
        return SearchOutcome::Exhausted;
      }
      if (check.decision == CompleteMappingDecision::Abort) {
        ++result_.stats.postMappingAbort;
        if (check.reasonCode == "budget_rf")
          ++result_.stats.rfBudgetExceeded;
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR,
                      check.reasonCode + (check.message.empty() ? "" : ": " + check.message), ii_);
        mapping_.reset();
        if (check.reasonCode.starts_with("budget"))
          return SearchOutcome::BudgetExceeded;
        if (check.reasonCode.starts_with("verification"))
          return SearchOutcome::VerificationFailure;
        return SearchOutcome::InternalError;
      }
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
      if (result_.stats.nodeCandidateAttempts >= limits_.nodeCandidateAttempts) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_II_BUDGET_SHARE_EXHAUSTED,
                      "per-II node-candidate budget share was exhausted", ii_, node);
        return SearchOutcome::PerIIBudgetExceeded;
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
      if (childOutcome == SearchOutcome::PerIIBudgetExceeded ||
          childOutcome == SearchOutcome::BudgetExceeded ||
          childOutcome == SearchOutcome::RouteBudgetExceeded ||
          childOutcome == SearchOutcome::VerificationFailure ||
          childOutcome == SearchOutcome::InternalError ||
          childOutcome == SearchOutcome::PostMappingAbort)
        return childOutcome;
      if (result_.stats.backtracks >= limits_.backtracks) {
        addDiagnostic(result_, ModuloMapperDiagnosticCode::MAP_II_BUDGET_SHARE_EXHAUSTED,
                      "per-II backtrack budget share was exhausted", ii_, node);
        return SearchOutcome::PerIIBudgetExceeded;
      }
      ++result_.stats.backtracks;
    }
    return SearchOutcome::Exhausted;
  }
};

std::uint64_t fairShareLimit(std::uint64_t maximum, std::uint64_t used,
                             std::uint64_t attemptsRemaining) {
  if (used >= maximum)
    return used;
  const auto remaining = maximum - used;
  // Spend at most half of the remaining global budget below the final II. This
  // favors low-II solutions while guaranteeing deterministic search capacity
  // for a later, less constrained II.
  const auto share = attemptsRemaining == 1 ? remaining : remaining / 2 + remaining % 2;
  return used + share;
}

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
  case ModuloMapperDiagnosticCode::MAP_POST_MAPPING_REJECTED:
    return "MAP_POST_MAPPING_REJECTED";
  case ModuloMapperDiagnosticCode::MAP_II_BUDGET_SHARE_EXHAUSTED:
    return "MAP_II_BUDGET_SHARE_EXHAUSTED";
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
  output << "\ncompleted modulo mappings: " << stats.completedModuloMappings
         << "\npost-mapping rejected: " << stats.postMappingRejected
         << "\nRF rejected: " << stats.rfRejected
         << "\nRF budget exceeded: " << stats.rfBudgetExceeded;
  output << "\nMII: " << mii << "\nsafe II: " << safeII
         << "\nsolution kind: " << solutionKind
         << "\nsuppressed diagnostics: " << suppressedDiagnostics;
  for (const auto& diagnostic : diagnostics)
    output << '\n' << toString(diagnostic.code) << ": " << diagnostic.message;
  return output.str();
}

std::string ModuloMapperResult::toJson() const {
  Json root = {{"schema", "cgra.modulo_mapper.result.v1"},
               {"status", toString(status)},
               {"mii", mii},
               {"safe_ii", safeII},
               {"best_known_ii", bestKnownII},
               {"solution_kind", solutionKind},
               {"fallback_invoked", fallbackInvoked},
               {"fallback_attempts", fallbackAttempts},
               {"fallback_local_repairs", fallbackLocalRepairs},
               {"fallback_schedule_growth", fallbackScheduleGrowth},
               {"suppressed_diagnostics", suppressedDiagnostics},
               {"fallback_failure_reason", fallbackFailureReason},
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
                 {"total_route_state_expansions", stats.totalRouteStateExpansions},
                 {"completed_modulo_mappings", stats.completedModuloMappings},
                 {"post_mapping_rejected", stats.postMappingRejected},
                 {"stage_rejected", stats.stageRejected},
                 {"rf_rejected", stats.rfRejected},
                 {"rf_budget_exceeded", stats.rfBudgetExceeded},
                 {"rf_rejected_by_ii", Json::object()},
                 {"rf_rejected_by_reason", Json::object()},
                 {"post_mapping_abort", stats.postMappingAbort}}},
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
  for (const auto& [ii, count] : stats.rfRejectedByII)
    root["stats"]["rf_rejected_by_ii"][std::to_string(ii)] = count;
  for (const auto& [reason, count] : stats.rfRejectedByReason)
    root["stats"]["rf_rejected_by_reason"][reason] = count;
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
  result.mii = mii.mii;
  const auto startII = std::max(mii.mii, options.minII == 0 ? mii.mii : options.minII);
  const auto configuredMaxII = options.maxII == 0 ? startII : options.maxII;
  const auto fallbackLimit = options.feasibilityFallback.maxSafeII;
  const auto maxII = fallbackLimit == 0 ? configuredMaxII : std::max(configuredMaxII, fallbackLimit);
  if (maxII < startII) {
    result.status = ModuloMapperStatus::NoMappingWithinIILimit;
    addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
                  "maxII is below the analyzer lower bound", mii.mii);
    return result;
  }

  bool perIIBudgetExceeded = false;
  const auto lowIIEnd = std::min<std::uint64_t>(
      maxII, static_cast<std::uint64_t>(startII) + options.feasibilityFallback.lowIIWindow);
  for (std::uint64_t ii = startII; ii <= lowIIEnd; ++ii) {
    ++result.stats.iiAttempts;
    const auto currentII = static_cast<std::uint32_t>(ii);
    const auto attemptsRemaining = static_cast<std::uint64_t>(maxII) - ii + 1;
    const SearchLimits limits{
        fairShareLimit(options.budget.maxNodeCandidateAttempts, result.stats.nodeCandidateAttempts,
                       attemptsRemaining),
        fairShareLimit(options.budget.maxBacktracks, result.stats.backtracks, attemptsRemaining),
        fairShareLimit(options.budget.maxRouteSearchCalls, result.stats.routeSearchCalls,
                       attemptsRemaining)};
    MappingSearchState state(dfg, target, options, limits, currentII, result);
    const auto outcome = state.run();
    result.stats.finalII = currentII;
    if (outcome == SearchOutcome::Success) {
      result.status = ModuloMapperStatus::Success;
      result.mapping = state.mapping();
      result.bestKnownII = currentII;
      result.safeII = currentII;
      result.solutionKind = "low_ii_search";
      result.fallbackScheduleGrowth = 0;
      result.fallbackLocalRepairs = 0;
      return result;
    }
    if (outcome == SearchOutcome::PerIIBudgetExceeded) {
      perIIBudgetExceeded = true;
      continue;
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

  if (options.objective == MappingObjective::FindAnyFeasible &&
      options.feasibilityFallback.enabled && lowIIEnd < maxII) {
    result.fallbackInvoked = true;
    ConstructiveModuloMapperOptions constructiveOptions;
    constructiveOptions.minII = static_cast<std::uint32_t>(lowIIEnd + 1);
    constructiveOptions.maxSafeII = maxII;
    constructiveOptions.maxLocalRepairs = options.feasibilityFallback.maxLocalRepairs;
    constructiveOptions.seed = options.feasibilityFallback.seed;
    constructiveOptions.completeMappingChecker = options.completeMappingChecker;
    constructiveOptions.budget = options.budget;
    constructiveOptions.routeOptions = options.routeOptions;
    auto constructive = mapConstructively(dfg, target, constructiveOptions);

    result.fallbackAttempts += constructive.stats.iiAttempts;
    result.fallbackLocalRepairs += constructive.fallbackLocalRepairs;
    result.fallbackScheduleGrowth = constructive.fallbackScheduleGrowth;
    result.stats.iiAttempts += constructive.stats.iiAttempts;
    result.stats.nodeCandidateAttempts += constructive.stats.nodeCandidateAttempts;
    result.stats.successfulPlacements += constructive.stats.successfulPlacements;
    result.stats.rejectedPlacements += constructive.stats.rejectedPlacements;
    result.stats.routeSearchCalls += constructive.stats.routeSearchCalls;
    result.stats.routeSuccesses += constructive.stats.routeSuccesses;
    result.stats.routeNoPaths += constructive.stats.routeNoPaths;
    result.stats.routeBudgetExceeded += constructive.stats.routeBudgetExceeded;
    result.stats.totalRouteStateExpansions += constructive.stats.totalRouteStateExpansions;
    result.stats.completedModuloMappings += constructive.stats.completedModuloMappings;
    result.stats.postMappingRejected += constructive.stats.postMappingRejected;
    result.stats.stageRejected += constructive.stats.stageRejected;
    result.stats.rfRejected += constructive.stats.rfRejected;
    for (const auto& [ii, count] : constructive.stats.rfRejectedByII)
      result.stats.rfRejectedByII[ii] += count;
    for (const auto& [reason, count] : constructive.stats.rfRejectedByReason)
      result.stats.rfRejectedByReason[reason] += count;
    result.stats.finalII = constructive.stats.finalII;
    result.diagnostics.insert(result.diagnostics.end(), constructive.diagnostics.begin(),
                              constructive.diagnostics.end());
    result.suppressedDiagnostics += constructive.suppressedDiagnostics;
    if (constructive.ok()) {
      result.status = ModuloMapperStatus::Success;
      result.mapping = std::move(constructive.mapping);
      result.safeII = constructive.safeII;
      result.bestKnownII = constructive.bestKnownII;
      result.solutionKind = "constructive_fallback";
      return result;
    }
    result.fallbackFailureReason = constructive.fallbackFailureReason;
    result.status = constructive.status;
    if (result.status == ModuloMapperStatus::Success)
      result.status = ModuloMapperStatus::NoMappingWithinIILimit;
    return result;
  }

  if (perIIBudgetExceeded) {
    result.status = ModuloMapperStatus::BudgetExceeded;
    return result;
  }
  result.status = ModuloMapperStatus::NoMappingWithinIILimit;
  addDiagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
                "all II values through maxII were fully exhausted");
  return result;
}

} // namespace cgra::mapping
