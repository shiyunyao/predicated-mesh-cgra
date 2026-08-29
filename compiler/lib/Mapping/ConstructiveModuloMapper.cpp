// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ConstructiveModuloMapper.h"

#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Mapping/PartialRFEventAnalysis.h"
#include "cgra/Mapping/ModuloResourceModel.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cgra::mapping {
namespace {

using NodeId = target::TargetNodeId;
using EdgeId = target::TargetEdgeId;

void diagnostic(ModuloMapperResult& result, ModuloMapperDiagnosticCode code, std::string message,
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

struct EdgeReservation {
  EdgeId id = 0;
  ReservationDelta reservation;
  MappedDependence dependence;
  std::optional<RFPortReservationDelta> rfPorts;
};

struct CandidateState {
  const target::TargetDFG& dfg;
  const TargetModel& target;
  const ConstructiveModuloMapperOptions& options;
  std::uint32_t ii;
  ModuloResourceModel resources;
  ResourceReservationTable reservations;
  RFPortReservationTable rfPortReservations;
  std::map<NodeId, NodePlacement> moduloPlacements;
  std::map<NodeId, AbsoluteNodePlacement> absolutePlacements;
  std::map<EdgeId, EdgeReservation> edges;
  ConstructiveSchedule schedule;
  ConstructiveModuloMapperStats stats;

  CandidateState(const target::TargetDFG& graph, const TargetModel& model,
                 const ConstructiveModuloMapperOptions& mapperOptions, std::uint32_t period)
      : dfg(graph), target(model), options(mapperOptions), ii(period), resources(model, period),
        reservations(resources), rfPortReservations(model) {
    schedule.period = period;
  }

  std::vector<ResourceId> routeResources(const TransportPlan& plan,
                                         const NodePlacement& producer) const {
    std::vector<ResourceId> resourcesUsed;
    const ModuloTimeDomain time(ii);
    for (const auto& action : plan.actions) {
      if (const auto* link = std::get_if<LinkStep>(&action)) {
        const auto slot = time.advance(producer.issueSlot, link->elapsedFromProducerIssue);
        const auto resource = resources.linkResource(plan.domain, link->source, link->direction,
                                                      slot);
        if (!resource)
          throw std::logic_error("constructive route references an unavailable link");
        if (std::ranges::find(resourcesUsed, *resource) != resourcesUsed.end())
          throw std::logic_error("constructive route reuses a link resource");
        resourcesUsed.push_back(*resource);
      }
    }
    return resourcesUsed;
  }

  bool realizeEdge(EdgeId edgeId) {
    if (edges.contains(edgeId))
      return true;
    const auto& edge = dfg.edge(edgeId);
    if (!moduloPlacements.contains(edge.src) || !moduloPlacements.contains(edge.dst))
      return true;
    const auto& producer = moduloPlacements.at(edge.src);
    const auto& consumer = moduloPlacements.at(edge.dst);
    if (edge.kind() == ir::Edge::Kind::Memory) {
      const auto separation = target.memoryDependenceSeparation(
          std::get<ir::MemoryEdgeInfo>(edge.info).dependence);
      edges.emplace(edgeId,
                    EdgeReservation{edgeId, {},
                                    MappedDependence{edgeId, edge.kind(), separation, std::nullopt},
                                    std::nullopt});
      schedule.transports.emplace(edgeId,
                                  AbsoluteTransport{edgeId,
                                                    absolutePlacements.at(edge.src).issueCycle,
                                                    absolutePlacements.at(edge.dst).issueCycle,
                                                    {}});
      return true;
    }
    auto routeOptions = options.routeOptions;
    routeOptions.budget = options.budget.perRouteBudget;
    ++stats.routeSearchCalls;
    const auto route = ModuloRouteSearch::search(dfg, target, resources, reservations,
                                                 {edgeId, producer, consumer}, routeOptions);
    stats.routeStateExpansions += route.stats.stateExpansions;
    if (route.status == RouteSearchStatus::NoPath) {
      ++stats.routeNoPaths;
      return false;
    }
    if (route.status == RouteSearchStatus::BudgetExceeded) {
      ++stats.routeBudgetExceeded;
      return false;
    }
    if (!route.ok())
      return false;
    ++stats.routeSuccesses;
    const auto dependence = MappedDependence{edgeId, edge.kind(),
                                             route.plan->requiredSeparationCycles, route.plan};
    std::optional<RFPortReservationDelta> rfReservation;
    if (options.rfPortAware.enabled &&
        (options.rfPortAware.reserveExplicitHoldEvents ||
         options.rfPortAware.reserveMandatoryTerminalEvents)) {
      const auto chain = derivePartialStorageChain(
          dfg, target, edge, producer, consumer, dependence, ii,
          options.rfPortAware.reserveMandatoryTerminalEvents);
      if (!chain.definiteEvents.empty()) {
        ++stats.rfPortMatchCalls;
        const auto portResult = rfPortReservations.tryReserve(chain.definiteEvents);
        if (!portResult.ok()) {
          ++stats.rfPortMatchFailures;
          if (portResult.status == RFPortReservationStatus::ReadCapacityExceeded)
            ++stats.rfReadPortEarlyRejects;
          else if (portResult.status == RFPortReservationStatus::WriteCapacityExceeded)
            ++stats.rfWritePortEarlyRejects;
          else if (portResult.status == RFPortReservationStatus::WriteSourceCompatibilityFailure)
            ++stats.rfWriteSourceEarlyRejects;
          return false;
        }
        stats.rfPortEventsCommitted += chain.definiteEvents.size();
        rfReservation = *portResult.delta;
      }
    }
    const auto ids = routeResources(*route.plan, producer);
    const auto reservation = reservations.tryReserve(ids, {ReservationOwnerKind::Edge, edgeId});
    if (!reservation) {
      if (rfReservation) {
        rfPortReservations.undo(*rfReservation);
        ++stats.rfPortRollbackCount;
      }
      return false;
    }
    edges.emplace(edgeId, EdgeReservation{edgeId, *reservation, dependence, rfReservation});
    schedule.transports.emplace(edgeId,
                                AbsoluteTransport{edgeId,
                                                  absolutePlacements.at(edge.src).issueCycle,
                                                  absolutePlacements.at(edge.dst).issueCycle,
                                                  *route.plan});
    return true;
  }

  void rollbackNode(NodeId node, const std::vector<EdgeId>& realized) {
    for (auto it = realized.rbegin(); it != realized.rend(); ++it) {
      const auto edge = edges.find(*it);
      if (edge == edges.end())
        continue;
      if (!edge->second.reservation.resources.empty())
        reservations.undo(edge->second.reservation);
      if (edge->second.rfPorts) {
        rfPortReservations.undo(*edge->second.rfPorts);
        ++stats.rfPortRollbackCount;
      }
      edges.erase(edge);
      schedule.transports.erase(*it);
    }
    const auto placement = moduloPlacements.find(node);
    if (placement != moduloPlacements.end()) {
      try {
        const auto footprint = resources.operationFootprint(dfg.node(node), placement->second.tile,
                                                            placement->second.issueSlot);
        reservations.undo({{ReservationOwnerKind::Node, node}, footprint});
      } catch (const std::exception&) {
      }
      moduloPlacements.erase(placement);
    }
    absolutePlacements.erase(node);
    schedule.placements.erase(node);
  }

  std::uint64_t earliest(NodeId node) const {
    std::uint64_t result = 0;
    for (const auto& edge : dfg.edges()) {
      if (edge.dst != node || !absolutePlacements.contains(edge.src))
        continue;
      std::uint64_t separation = 0;
      if (edge.kind() == ir::Edge::Kind::Memory) {
        separation = target.memoryDependenceSeparation(
            std::get<ir::MemoryEdgeInfo>(edge.info).dependence);
      } else {
        separation = dfg.node(edge.src).producerOutputReadyOffset.value_or(0U);
      }
      const auto sourceTime = absolutePlacements.at(edge.src).issueCycle;
      if (sourceTime > std::numeric_limits<std::uint64_t>::max() - separation)
        return std::numeric_limits<std::uint64_t>::max();
      const auto readyTime = sourceTime + separation;
      const auto recurrenceOffset = static_cast<std::uint64_t>(edge.distance) * ii;
      // src(i) -> dst(i + distance) means
      // dst_time + distance * II >= src_time + separation.  Adding the
      // recurrence offset here would delay the consumer by an extra iteration
      // and manufacture the RF lifetime that this scheduler is intended to
      // avoid.
      const auto candidate = readyTime > recurrenceOffset ? readyTime - recurrenceOffset : 0;
      result = std::max(result, candidate);
    }
    return result;
  }

  bool place(NodeId node) {
    const auto& targetNode = dfg.node(node);
    auto tiles = target.compatibleTiles(targetNode.operation);
    std::ranges::sort(tiles);
    const auto start = earliest(node);
    // One period of candidate slots is enough to preserve modulo resource
    // legality. A longer absolute schedule is represented by its stage.
    for (std::uint64_t cycle = start; cycle < start + ii; ++cycle) {
      for (const auto& [row, col] : tiles) {
        const TileCoord tile{row, col};
        const auto slot = ModuloSlot(static_cast<std::uint32_t>(cycle % ii));
        std::vector<ResourceId> footprint;
        try {
          footprint = resources.operationFootprint(targetNode, tile, slot);
        } catch (const std::exception&) {
          ++stats.placementRepairs;
          continue;
        }
        const auto reservation = reservations.tryReserve(footprint, {ReservationOwnerKind::Node, node});
        if (!reservation) {
          ++stats.placementRepairs;
          continue;
        }
        moduloPlacements.emplace(node, NodePlacement{node, tile, slot});
        ++stats.successfulPlacements;
        absolutePlacements.emplace(node, AbsoluteNodePlacement{node, tile, cycle});
        schedule.placements.emplace(node, AbsoluteNodePlacement{node, tile, cycle});
        std::vector<EdgeId> realized;
        bool routesOkay = true;
        for (const auto& edge : dfg.edges()) {
          if (edge.src != node && edge.dst != node)
            continue;
          if (moduloPlacements.contains(edge.src) && moduloPlacements.contains(edge.dst) &&
              !edges.contains(edge.id)) {
            if (!realizeEdge(edge.id)) {
              routesOkay = false;
              ++stats.routeRepairs;
              break;
            }
            realized.push_back(edge.id);
          }
        }
        if (routesOkay)
          return true;
        rollbackNode(node, realized);
      }
    }
    return false;
  }

  std::optional<ModuloMapping> finish(ModuloMapperResult& result) {
    try {
      ModuloMappingBuilder builder(dfg, ii);
      for (const auto& [node, placement] : moduloPlacements)
        builder.place(node, placement.tile, placement.issueSlot);
      for (const auto& edge : dfg.edges()) {
        const auto found = edges.find(edge.id);
        if (found == edges.end())
          return std::nullopt;
        if (found->second.dependence.transport)
          builder.setTransport(edge.id, *found->second.dependence.transport);
        else
          builder.setMemorySeparation(edge.id, found->second.dependence.requiredSeparationCycles);
      }
      auto mapping = builder.finish();
      const auto report = ModuloMappingVerifier::verify(dfg, target, mapping);
      if (!report.ok()) {
        diagnostic(result, ModuloMapperDiagnosticCode::MAP_FINAL_VERIFICATION_FAILED,
                   report.format(), ii);
        return std::nullopt;
      }
      return mapping;
    } catch (const std::exception& error) {
      diagnostic(result, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR,
                 std::string("constructive mapping materialization failed: ") + error.what(), ii);
      return std::nullopt;
    }
  }
};

std::uint64_t criticality(const target::TargetDFG& dfg, NodeId node,
                          std::map<NodeId, std::uint64_t>& memo,
                          std::set<NodeId>& active) {
  if (const auto found = memo.find(node); found != memo.end())
    return found->second;
  if (!active.insert(node).second)
    return 0;
  std::uint64_t value = 1;
  for (const auto& edge : dfg.edges())
    if (edge.dst == node && edge.distance == 0 && edge.kind() != ir::Edge::Kind::Memory)
      value = std::max(value, criticality(dfg, edge.src, memo, active) + 1);
  active.erase(node);
  return memo[node] = value;
}

std::vector<NodeId> orderedNodes(const target::TargetDFG& dfg, const TargetModel& target) {
  std::map<NodeId, std::uint64_t> critical;
  for (const auto& node : dfg.nodes()) {
    std::set<NodeId> active;
    criticality(dfg, node.id, critical, active);
  }
  const auto priority = [&](NodeId lhs, NodeId rhs) {
    const auto& left = dfg.node(lhs);
    const auto& right = dfg.node(rhs);
    const auto leftIngress = left.operation.starts_with("PASS") ? 0U : 1U;
    const auto rightIngress = right.operation.starts_with("PASS") ? 0U : 1U;
    const auto leftMemory = left.executionClass == TargetExecutionClass::LSU ? 0U : 1U;
    const auto rightMemory = right.executionClass == TargetExecutionClass::LSU ? 0U : 1U;
    const auto leftDomain = target.compatibleTiles(left.operation).size();
    const auto rightDomain = target.compatibleTiles(right.operation).size();
    const auto fanout = [&](NodeId node) {
      return std::ranges::count_if(dfg.edges(), [node](const auto& edge) {
        return edge.src == node && edge.distance == 0;
      });
    };
    return std::tuple{leftIngress, leftMemory, std::numeric_limits<std::uint64_t>::max() - critical[lhs],
                      leftDomain, std::numeric_limits<std::uint64_t>::max() - fanout(lhs), lhs} <
           std::tuple{rightIngress, rightMemory,
                      std::numeric_limits<std::uint64_t>::max() - critical[rhs], rightDomain,
                      std::numeric_limits<std::uint64_t>::max() - fanout(rhs), rhs};
  };

  // A priority-only sort can place a consumer before its distance-zero
  // producer.  Constructive scheduling needs the producer's absolute issue
  // time first, so use a deterministic Kahn order and apply the placement
  // priority only among currently-ready nodes. Loop-carried and memory edges
  // are intentionally excluded: they constrain separation, not same-iteration
  // value readiness.
  std::map<NodeId, std::uint32_t> indegree;
  std::map<NodeId, std::vector<NodeId>> successors;
  for (const auto& node : dfg.nodes())
    indegree.emplace(node.id, 0);
  for (const auto& edge : dfg.edges()) {
    if (edge.distance != 0 || edge.kind() == ir::Edge::Kind::Memory)
      continue;
    ++indegree[edge.dst];
    successors[edge.src].push_back(edge.dst);
  }
  for (auto& [_, users] : successors)
    std::ranges::sort(users);
  struct ReadyCompare {
    decltype(priority)* compare;
    bool operator()(NodeId lhs, NodeId rhs) const {
      if (lhs == rhs)
        return false;
      return (*compare)(lhs, rhs);
    }
  };
  ReadyCompare compare{&priority};
  std::set<NodeId, ReadyCompare> ready(compare);
  for (const auto& [node, degree] : indegree)
    if (degree == 0)
      ready.insert(node);
  std::vector<NodeId> result;
  result.reserve(dfg.nodes().size());
  while (!ready.empty()) {
    const auto node = *ready.begin();
    ready.erase(ready.begin());
    result.push_back(node);
    for (const auto user : successors[node])
      if (--indegree[user] == 0)
        ready.insert(user);
  }
  // A valid TargetDFG should not contain a distance-zero data cycle. Keep the
  // function total for diagnostics by appending any unexpected remainder in
  // the same deterministic priority order; the final verifier will reject it.
  if (result.size() != dfg.nodes().size()) {
    std::vector<NodeId> remainder;
    for (const auto& [node, degree] : indegree)
      if (degree != 0)
        remainder.push_back(node);
    std::ranges::sort(remainder, priority);
    result.insert(result.end(), remainder.begin(), remainder.end());
  }
  return result;
}

std::uint32_t safeCap(std::uint32_t start, const ConstructiveModuloMapperOptions& options) {
  if (options.maxSafeII != 0)
    return options.maxSafeII;
  // No finite control-memory upper bound is exposed by TargetModel yet. This
  // documented implementation cap prevents an unbounded coverage run; a cap
  // hit is reported as mapper-incomplete, never as resource infeasible.
  return std::max<std::uint32_t>(64, start + 32);
}

} // namespace

ModuloMapperResult mapConstructively(const target::TargetDFG& dfg, const TargetModel& target,
                                     const ConstructiveModuloMapperOptions& options) {
  ModuloMapperResult result;
  const auto targetReport = target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    result.status = ModuloMapperStatus::InvalidTargetDFG;
    diagnostic(result, ModuloMapperDiagnosticCode::MAP_INVALID_TARGET_DFG, targetReport.format());
    return result;
  }
  const auto mii = analysis::MIIAnalyzer::analyze(dfg, target);
  if (!mii.ok() || mii.mii == 0 || mii.mii == std::numeric_limits<std::uint32_t>::max()) {
    result.status = ModuloMapperStatus::MIIAnalysisFailure;
    diagnostic(result, ModuloMapperDiagnosticCode::MAP_MII_ANALYSIS_FAILED,
               mii.ok() ? "MII analyzer returned an invalid starting II" : mii.format());
    return result;
  }
  result.mii = mii.mii;
  result.stats.startingMII = mii.mii;
  const auto start = std::max(mii.mii, options.minII);
  const auto cap = safeCap(start, options);
  if (cap < start) {
    result.status = ModuloMapperStatus::NoMappingWithinIILimit;
    diagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
               "constructive safe-II cap is below MII", mii.mii);
    return result;
  }
  const auto order = orderedNodes(dfg, target);
  for (std::uint32_t ii = start; ii <= cap; ++ii) {
    ++result.stats.iiAttempts;
    CandidateState candidate(dfg, target, options, ii);
    ++candidate.stats.scheduleAttempts;
    bool placed = true;
    for (const auto node : order) {
      if (!candidate.place(node)) {
        placed = false;
        break;
      }
    }
    result.stats.nodeCandidateAttempts += candidate.stats.placementRepairs + dfg.nodes().size();
    result.stats.successfulPlacements += candidate.stats.successfulPlacements;
    result.stats.rejectedPlacements += candidate.stats.placementRepairs;
    result.stats.routeSearchCalls += candidate.stats.routeSearchCalls;
    result.stats.routeSuccesses += candidate.stats.routeSuccesses;
    result.stats.routeNoPaths += candidate.stats.routeNoPaths;
    result.stats.routeBudgetExceeded += candidate.stats.routeBudgetExceeded;
    result.stats.totalRouteStateExpansions += candidate.stats.routeStateExpansions;
    result.fallbackLocalRepairs += candidate.stats.placementRepairs + candidate.stats.routeRepairs;
    if (!placed) {
      ++result.fallbackAttempts;
      result.fallbackScheduleGrowth = ii - start;
      continue;
    }
    const auto mapping = candidate.finish(result);
    if (!mapping) {
      ++result.stats.postMappingRejected;
      ++result.fallbackAttempts;
      continue;
    }
    ++result.stats.completedModuloMappings;
    if (options.completeMappingChecker) {
      const auto check = options.completeMappingChecker(dfg, target, *mapping);
      if (check.decision == CompleteMappingDecision::Reject) {
        ++result.stats.postMappingRejected;
        if (check.reasonCode.starts_with("stage")) {
          ++result.stats.stageRejected;
          ++candidate.stats.stageRepairs;
        }
        if (check.reasonCode.starts_with("rf")) {
          ++result.stats.rfRejected;
          ++result.stats.rfRejectedByII[ii];
          ++result.stats.rfRejectedByReason[check.reasonCode];
          ++candidate.stats.rfRepairs;
        }
        diagnostic(result, ModuloMapperDiagnosticCode::MAP_POST_MAPPING_REJECTED,
                   check.reasonCode + (check.message.empty() ? "" : ": " + check.message), ii);
        result.fallbackLocalRepairs += candidate.stats.stageRepairs + candidate.stats.rfRepairs;
        ++result.fallbackAttempts;
        continue;
      }
      if (check.decision == CompleteMappingDecision::Abort) {
        result.status = ModuloMapperStatus::InternalError;
        diagnostic(result, ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR,
                   check.reasonCode + (check.message.empty() ? "" : ": " + check.message), ii);
        return result;
      }
    }
    result.status = ModuloMapperStatus::Success;
    result.mapping = mapping;
    result.safeII = ii;
    result.bestKnownII = ii;
    result.stats.finalII = ii;
    result.solutionKind = "constructive_fallback";
    result.fallbackScheduleGrowth = ii - start;
    return result;
  }
  result.status = ModuloMapperStatus::NoMappingWithinIILimit;
  result.fallbackFailureReason = "constructive implementation cap reached without a verified candidate";
  diagnostic(result, ModuloMapperDiagnosticCode::MAP_NO_MAPPING_WITHIN_II_LIMIT,
             result.fallbackFailureReason, cap);
  result.stats.finalII = cap;
  return result;
}

} // namespace cgra::mapping
