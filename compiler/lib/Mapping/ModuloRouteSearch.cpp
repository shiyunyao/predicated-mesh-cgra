// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloRouteSearch.h"

#include "cgra/Target/TargetDFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cgra::mapping {
namespace {

using Json = nlohmann::json;
using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

std::string_view directionName(Direction direction) noexcept {
  switch (direction) {
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::East:
    return "east";
  case Direction::West:
    return "west";
  }
  return "north";
}

std::string_view domainName(NetworkDomain domain) noexcept {
  return domain == NetworkDomain::Data ? "data" : "predicate";
}

void addDiagnostic(RouteSearchResult& result, RouteSearchDiagnosticCode code, std::string message,
                   std::optional<EdgeId> edge = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), edge});
}

struct RouteState {
  TileCoord tile;
  ModuloSlot slot;
};

struct Cost {
  std::uint64_t elapsed = std::numeric_limits<std::uint64_t>::max();
  std::uint32_t holds = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t hops = std::numeric_limits<std::uint32_t>::max();

  friend bool operator==(const Cost&, const Cost&) = default;
  friend bool operator<(const Cost& lhs, const Cost& rhs) noexcept {
    if (lhs.elapsed != rhs.elapsed)
      return lhs.elapsed < rhs.elapsed;
    if (lhs.holds != rhs.holds)
      return lhs.holds < rhs.holds;
    return lhs.hops < rhs.hops;
  }
};

enum class PredecessorKind {
  None,
  Link,
  Hold,
};

struct Predecessor {
  std::size_t previous = 0;
  PredecessorKind kind = PredecessorKind::None;
  Direction direction = Direction::North;
};

struct QueueEntry {
  Cost cost;
  std::size_t state = 0;
  std::uint64_t sequence = 0;
};

struct QueueEntryCompare {
  bool operator()(const QueueEntry& lhs, const QueueEntry& rhs) const noexcept {
    if (lhs.cost != rhs.cost)
      return rhs.cost < lhs.cost;
    if (lhs.state != rhs.state)
      return lhs.state > rhs.state;
    return lhs.sequence > rhs.sequence;
  }
};

class RouteSearchImpl {
public:
  RouteSearchImpl(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                  const ModuloResourceModel& resources,
                  const ResourceReservationTable& reservations, const RouteSearchRequest& request,
                  const RouteSearchOptions& options)
      : dfg_(dfg), target_(target), resources_(resources), reservations_(reservations),
        request_(request), options_(options), time_(resources.ii()) {}

  RouteSearchResult run() {
    if (!validateInput())
      return result_;

    const auto rows = target_.array().rows;
    const auto cols = target_.array().cols;
    const auto stateCount = static_cast<std::uint64_t>(rows) * cols * resources_.ii();
    if (stateCount == 0 || stateCount > std::numeric_limits<std::size_t>::max()) {
      result_.status = RouteSearchStatus::InternalError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                    "routing state universe does not fit in the host address space", request_.edge);
      return result_;
    }

    best_.assign(static_cast<std::size_t>(stateCount), Cost{});
    predecessors_.assign(static_cast<std::size_t>(stateCount), Predecessor{});
    finalized_.assign(static_cast<std::size_t>(stateCount), false);

    const auto producerReady = *dfg_.node(request_.producer.node).producerOutputReadyOffset;
    const auto startSlot = time_.advance(request_.producer.issueSlot, producerReady);
    const auto start = stateId({request_.producer.tile, startSlot});
    best_[start] = {producerReady, 0, 0};

    if (!push({best_[start], start, sequence_++})) {
      result_.status = RouteSearchStatus::BudgetExceeded;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_BUDGET_EXCEEDED,
                    "route search queue-push budget was exhausted", request_.edge);
      return result_;
    }

    if (request_.producer.tile == request_.consumer.tile) {
      if (!options_.allowVirtualHold) {
        return noPath("same-tile dependence requires virtual local storage");
      }
      if (producerReady == std::numeric_limits<std::uint32_t>::max()) {
        result_.status = RouteSearchStatus::InternalError;
        addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                      "producer readiness overflows TransportPlan elapsed time", request_.edge);
        return result_;
      }
      TransportPlan plan;
      plan.edge = request_.edge;
      plan.domain = domain_;
      plan.requiredSeparationCycles = producerReady + 1;
      plan.actions.emplace_back(
          VirtualHold{domain_, request_.producer.tile, producerReady, producerReady + 1});
      result_.status = RouteSearchStatus::Success;
      result_.plan = std::move(plan);
      result_.stats.resultSeparation = producerReady + 1;
      result_.stats.resultHoldCycles = 1;
      return result_;
    }

    while (!queue_.empty()) {
      const auto current = queue_.top();
      queue_.pop();
      if (finalized_[current.state] || !(current.cost == best_[current.state]))
        continue;
      finalized_[current.state] = true;

      const auto state = stateFor(current.state);
      if (state.tile == request_.consumer.tile) {
        return success(current.state);
      }
      if (result_.stats.stateExpansions >= options_.budget.maxStateExpansions) {
        result_.status = RouteSearchStatus::BudgetExceeded;
        addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_BUDGET_EXCEEDED,
                      "route search state-expansion budget was exhausted", request_.edge);
        return result_;
      }
      ++result_.stats.stateExpansions;

      for (const auto direction :
           {Direction::North, Direction::East, Direction::South, Direction::West}) {
        if (!expandLink(current.state, current.cost, direction))
          return result_.status == RouteSearchStatus::BudgetExceeded ? result_ : internalFailure();
      }
      if (options_.allowVirtualHold && !expandHold(current.state, current.cost))
        return result_.status == RouteSearchStatus::BudgetExceeded ? result_ : internalFailure();
    }

    return noPath("finite modulo routing state graph was exhausted without reaching the consumer");
  }

private:
  bool validateInput() {
    const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg_, target_);
    if (!targetReport.ok()) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INVALID_TARGET_DFG,
                    "TargetDFG precondition failed: " + targetReport.format(), request_.edge);
      return false;
    }
    if (dfg_.targetName() != target_.name() || resources_.target().name() != target_.name()) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INVALID_TARGET_DFG,
                    "TargetDFG, resource model, and selected target disagree", request_.edge);
      return false;
    }
    if (resources_.ii() == 0) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_SLOT_OUT_OF_RANGE,
                    "route resource model has an invalid zero II", request_.edge);
      return false;
    }
    if (!dfg_.containsEdge(request_.edge)) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INVALID_EDGE,
                    "requested TargetDFG edge does not exist", request_.edge);
      return false;
    }
    const auto& edge = dfg_.edge(request_.edge);
    if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
      result_.status = RouteSearchStatus::UnsupportedEdge;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_EDGE_NOT_VALUE_CARRYING,
                    "memory dependences are ordering constraints and are not routed", edge.id);
      return false;
    }
    if (request_.producer.node != edge.src) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_PRODUCER_PLACEMENT_MISMATCH,
                    "producer placement does not identify the edge source", edge.id);
      return false;
    }
    if (request_.consumer.node != edge.dst) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_CONSUMER_PLACEMENT_MISMATCH,
                    "consumer placement does not identify the edge destination", edge.id);
      return false;
    }
    if (!validPlacement(request_.producer, edge.src, "producer") ||
        !validPlacement(request_.consumer, edge.dst, "consumer"))
      return false;

    const auto& producer = dfg_.node(edge.src);
    if (!producer.producerOutputReadyOffset) {
      result_.status = RouteSearchStatus::TargetContractError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_TARGET_TIMING_MISSING,
                    "producer operation has no output-ready timing contract", edge.id);
      return false;
    }
    domain_ = edge.kind() == cgra::ir::Edge::Kind::Predicate ? NetworkDomain::Predicate
                                                             : NetworkDomain::Data;
    const auto& network =
        domain_ == NetworkDomain::Predicate ? target_.predicateNetwork() : target_.dataNetwork();
    if (network.hopLatency == 0) {
      result_.status = RouteSearchStatus::TargetContractError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_TARGET_TIMING_MISSING,
                    "selected target network has zero hop latency", edge.id);
      return false;
    }
    return true;
  }

  bool validPlacement(const NodePlacement& placement, NodeId expected, std::string_view role) {
    if (placement.node != expected) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_,
                    role == "producer"
                        ? RouteSearchDiagnosticCode::ROUTE_PRODUCER_PLACEMENT_MISMATCH
                        : RouteSearchDiagnosticCode::ROUTE_CONSUMER_PLACEMENT_MISMATCH,
                    std::string(role) + " placement node ID is inconsistent", request_.edge);
      return false;
    }
    if (placement.tile.row >= target_.array().rows || placement.tile.col >= target_.array().cols) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_TILE_OUT_OF_RANGE,
                    std::string(role) + " placement tile is outside the target array",
                    request_.edge);
      return false;
    }
    if (placement.issueSlot.value() >= resources_.ii()) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_SLOT_OUT_OF_RANGE,
                    std::string(role) + " placement slot is outside [0, II)", request_.edge);
      return false;
    }
    const auto& node = dfg_.node(expected);
    if (!resources_.supportsOperation(placement.tile, node)) {
      result_.status = RouteSearchStatus::InvalidInput;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_OPERATION_UNSUPPORTED_ON_TILE,
                    std::string(role) + " operation is not supported on its placement tile",
                    request_.edge);
      return false;
    }
    return true;
  }

  std::size_t stateId(RouteState state) const noexcept {
    return (static_cast<std::size_t>(state.slot.value()) * target_.array().rows + state.tile.row) *
               target_.array().cols +
           state.tile.col;
  }

  RouteState stateFor(std::size_t id) const noexcept {
    const auto cols = target_.array().cols;
    const auto rowCount = static_cast<std::size_t>(target_.array().rows) * cols;
    const auto slot = id / rowCount;
    const auto within = id % rowCount;
    return {{static_cast<std::uint32_t>(within / cols), static_cast<std::uint32_t>(within % cols)},
            ModuloSlot(static_cast<std::uint32_t>(slot))};
  }

  bool push(QueueEntry entry) {
    if (result_.stats.queuePushes >= options_.budget.maxQueuePushes)
      return false;
    queue_.push(entry);
    ++result_.stats.queuePushes;
    result_.stats.maxQueueSize =
        std::max(result_.stats.maxQueueSize, static_cast<std::uint32_t>(queue_.size()));
    return true;
  }

  bool relaxTo(std::size_t previous, std::size_t destination, Cost cost, Predecessor predecessor) {
    if (finalized_[destination] || !(cost < best_[destination]))
      return true;
    best_[destination] = cost;
    predecessor.previous = previous;
    predecessors_[destination] = predecessor;
    if (!push({cost, destination, sequence_++})) {
      result_.status = RouteSearchStatus::BudgetExceeded;
      return false;
    }
    return true;
  }

  bool expandLink(std::size_t currentId, const Cost& current, Direction direction) {
    const auto state = stateFor(currentId);
    ++result_.stats.linkTransitionsConsidered;
    const auto nextTile = neighbor(state.tile, direction, target_);
    if (!nextTile) {
      ++result_.stats.invalidBorderLinks;
      return true;
    }
    const auto launchSlot = time_.advance(request_.producer.issueSlot, current.elapsed);
    const auto resource = resources_.linkResource(domain_, state.tile, direction, launchSlot);
    if (!resource) {
      ++result_.stats.invalidBorderLinks;
      return true;
    }
    if (!reservations_.isFree(*resource)) {
      ++result_.stats.blockedLinks;
      return true;
    }
    const auto& network =
        domain_ == NetworkDomain::Predicate ? target_.predicateNetwork() : target_.dataNetwork();
    if (current.elapsed > std::numeric_limits<std::uint64_t>::max() - network.hopLatency ||
        current.hops == std::numeric_limits<std::uint32_t>::max()) {
      result_.status = RouteSearchStatus::InternalError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                    "route elapsed time overflows the search cost type", request_.edge);
      return false;
    }
    const auto nextSlot = time_.advance(state.slot, network.hopLatency);
    const auto nextId = stateId({*nextTile, nextSlot});
    const Cost nextCost{current.elapsed + network.hopLatency, current.holds, current.hops + 1};
    if (!relaxTo(currentId, nextId, nextCost, {currentId, PredecessorKind::Link, direction}))
      return false;
    return true;
  }

  bool expandHold(std::size_t currentId, const Cost& current) {
    ++result_.stats.holdTransitionsConsidered;
    if (current.elapsed == std::numeric_limits<std::uint64_t>::max() ||
        current.holds == std::numeric_limits<std::uint32_t>::max()) {
      result_.status = RouteSearchStatus::InternalError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                    "route hold time overflows the search cost type", request_.edge);
      return false;
    }
    const auto state = stateFor(currentId);
    const auto nextSlot = time_.advance(state.slot, 1);
    const auto nextId = stateId({state.tile, nextSlot});
    const Cost nextCost{current.elapsed + 1, current.holds + 1, current.hops};
    if (!relaxTo(currentId, nextId, nextCost, {currentId, PredecessorKind::Hold, Direction::North}))
      return false;
    return true;
  }

  RouteSearchResult success(std::size_t goal) {
    auto plan = reconstruct(goal);
    if (!plan) {
      result_.status = RouteSearchStatus::InternalError;
      addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                    "route predecessor chain could not be reconstructed", request_.edge);
      return result_;
    }
    result_.status = RouteSearchStatus::Success;
    result_.stats.resultSeparation = plan->requiredSeparationCycles;
    for (const auto& action : plan->actions) {
      if (std::holds_alternative<LinkStep>(action))
        ++result_.stats.resultHopCount;
      else {
        const auto& hold = std::get<VirtualHold>(action);
        result_.stats.resultHoldCycles += hold.releaseElapsed - hold.captureElapsed;
      }
    }
    result_.plan = std::move(*plan);
    return result_;
  }

  std::optional<TransportPlan> reconstruct(std::size_t goal) {
    std::vector<std::pair<std::size_t, Predecessor>> reversed;
    std::size_t current = goal;
    const auto start =
        stateId({request_.producer.tile,
                 time_.advance(request_.producer.issueSlot,
                               *dfg_.node(request_.producer.node).producerOutputReadyOffset)});
    while (current != start) {
      if (current >= predecessors_.size() || predecessors_[current].kind == PredecessorKind::None ||
          reversed.size() > predecessors_.size())
        return std::nullopt;
      reversed.emplace_back(current, predecessors_[current]);
      current = predecessors_[current].previous;
    }
    std::reverse(reversed.begin(), reversed.end());
    TransportPlan plan;
    plan.edge = request_.edge;
    plan.domain = domain_;
    for (const auto& [child, predecessor] : reversed) {
      const auto parentState = stateFor(predecessor.previous);
      const auto& parentCost = best_[predecessor.previous];
      const auto& childCost = best_[child];
      if (predecessor.kind == PredecessorKind::Link) {
        if (parentCost.elapsed > std::numeric_limits<std::uint32_t>::max())
          return std::nullopt;
        plan.actions.emplace_back(LinkStep{domain_, parentState.tile, predecessor.direction,
                                           static_cast<std::uint32_t>(parentCost.elapsed)});
      } else {
        if (parentCost.elapsed > std::numeric_limits<std::uint32_t>::max() ||
            childCost.elapsed > std::numeric_limits<std::uint32_t>::max())
          return std::nullopt;
        const auto capture = static_cast<std::uint32_t>(parentCost.elapsed);
        const auto release = static_cast<std::uint32_t>(childCost.elapsed);
        if (!plan.actions.empty()) {
          if (auto* previousHold = std::get_if<VirtualHold>(&plan.actions.back());
              previousHold && previousHold->domain == domain_ &&
              previousHold->tile == parentState.tile && previousHold->releaseElapsed == capture) {
            previousHold->releaseElapsed = release;
            continue;
          }
        }
        plan.actions.emplace_back(VirtualHold{domain_, parentState.tile, capture, release});
      }
    }
    if (best_[goal].elapsed > std::numeric_limits<std::uint32_t>::max())
      return std::nullopt;
    plan.requiredSeparationCycles = static_cast<std::uint32_t>(best_[goal].elapsed);

    std::vector<ResourceId> seen;
    for (const auto& action : plan.actions) {
      const auto* link = std::get_if<LinkStep>(&action);
      if (!link)
        continue;
      const auto slot = time_.advance(request_.producer.issueSlot, link->elapsedFromProducerIssue);
      const auto resource = resources_.linkResource(domain_, link->source, link->direction, slot);
      if (!resource || std::find(seen.begin(), seen.end(), *resource) != seen.end()) {
        addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_SELF_CONFLICT,
                      "reconstructed route reuses an unavailable modulo link resource",
                      request_.edge);
        return std::nullopt;
      }
      seen.push_back(*resource);
    }
    return plan;
  }

  RouteSearchResult noPath(std::string message) {
    result_.status = RouteSearchStatus::NoPath;
    addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_NO_PATH, std::move(message),
                  request_.edge);
    return result_;
  }

  RouteSearchResult internalFailure() {
    result_.status = RouteSearchStatus::InternalError;
    addDiagnostic(result_, RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
                  "route search failed to enqueue a valid successor", request_.edge);
    return result_;
  }

  const cgra::target::TargetDFG& dfg_;
  const cgra::TargetModel& target_;
  const ModuloResourceModel& resources_;
  const ResourceReservationTable& reservations_;
  const RouteSearchRequest& request_;
  const RouteSearchOptions& options_;
  ModuloTimeDomain time_;
  NetworkDomain domain_ = NetworkDomain::Data;
  RouteSearchResult result_;
  std::vector<Cost> best_;
  std::vector<Predecessor> predecessors_;
  std::vector<bool> finalized_;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryCompare> queue_;
  std::uint64_t sequence_ = 0;
};

} // namespace

std::string_view toString(RouteSearchStatus status) noexcept {
  switch (status) {
  case RouteSearchStatus::Success:
    return "success";
  case RouteSearchStatus::NoPath:
    return "no_path";
  case RouteSearchStatus::BudgetExceeded:
    return "budget_exceeded";
  case RouteSearchStatus::InvalidInput:
    return "invalid_input";
  case RouteSearchStatus::UnsupportedEdge:
    return "unsupported_edge";
  case RouteSearchStatus::TargetContractError:
    return "target_contract_error";
  case RouteSearchStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(RouteSearchDiagnosticCode code) noexcept {
  switch (code) {
  case RouteSearchDiagnosticCode::ROUTE_INVALID_TARGET_DFG:
    return "ROUTE_INVALID_TARGET_DFG";
  case RouteSearchDiagnosticCode::ROUTE_INVALID_EDGE:
    return "ROUTE_INVALID_EDGE";
  case RouteSearchDiagnosticCode::ROUTE_EDGE_NOT_VALUE_CARRYING:
    return "ROUTE_EDGE_NOT_VALUE_CARRYING";
  case RouteSearchDiagnosticCode::ROUTE_PRODUCER_PLACEMENT_MISMATCH:
    return "ROUTE_PRODUCER_PLACEMENT_MISMATCH";
  case RouteSearchDiagnosticCode::ROUTE_CONSUMER_PLACEMENT_MISMATCH:
    return "ROUTE_CONSUMER_PLACEMENT_MISMATCH";
  case RouteSearchDiagnosticCode::ROUTE_SLOT_OUT_OF_RANGE:
    return "ROUTE_SLOT_OUT_OF_RANGE";
  case RouteSearchDiagnosticCode::ROUTE_TILE_OUT_OF_RANGE:
    return "ROUTE_TILE_OUT_OF_RANGE";
  case RouteSearchDiagnosticCode::ROUTE_OPERATION_UNSUPPORTED_ON_TILE:
    return "ROUTE_OPERATION_UNSUPPORTED_ON_TILE";
  case RouteSearchDiagnosticCode::ROUTE_TARGET_TIMING_MISSING:
    return "ROUTE_TARGET_TIMING_MISSING";
  case RouteSearchDiagnosticCode::ROUTE_NO_PATH:
    return "ROUTE_NO_PATH";
  case RouteSearchDiagnosticCode::ROUTE_BUDGET_EXCEEDED:
    return "ROUTE_BUDGET_EXCEEDED";
  case RouteSearchDiagnosticCode::ROUTE_INTERNAL_SELF_CONFLICT:
    return "ROUTE_INTERNAL_SELF_CONFLICT";
  case RouteSearchDiagnosticCode::ROUTE_INTERNAL_RECONSTRUCTION_ERROR:
    return "ROUTE_INTERNAL_RECONSTRUCTION_ERROR";
  case RouteSearchDiagnosticCode::ROUTE_INTERNAL_VERIFIER_REJECTED:
    return "ROUTE_INTERNAL_VERIFIER_REJECTED";
  }
  return "ROUTE_INTERNAL_RECONSTRUCTION_ERROR";
}

std::string RouteSearchResult::format() const {
  std::ostringstream output;
  output << toString(status) << " route search";
  if (stats.resultSeparation)
    output << " separation=" << *stats.resultSeparation;
  output << " expansions=" << stats.stateExpansions << " pushes=" << stats.queuePushes;
  for (const auto& diagnostic : diagnostics)
    output << '\n' << toString(diagnostic.code) << ": " << diagnostic.message;
  return output.str();
}

std::string RouteSearchResult::toJson(EdgeId edge) const {
  Json root = {{"schema", "cgra.modulo_route_search.result.v1"},
               {"status", toString(status)},
               {"edge", edge},
               {"stats",
                {{"state_expansions", stats.stateExpansions},
                 {"queue_pushes", stats.queuePushes},
                 {"link_transitions_considered", stats.linkTransitionsConsidered},
                 {"hold_transitions_considered", stats.holdTransitionsConsidered},
                 {"blocked_links", stats.blockedLinks},
                 {"invalid_border_links", stats.invalidBorderLinks},
                 {"max_queue_size", stats.maxQueueSize},
                 {"result_hop_count", stats.resultHopCount},
                 {"result_hold_cycles", stats.resultHoldCycles}}},
               {"diagnostics", Json::array()}};
  if (stats.resultSeparation)
    root["stats"]["result_separation"] = *stats.resultSeparation;
  if (plan) {
    Json actions = Json::array();
    for (const auto& action : plan->actions) {
      if (const auto* link = std::get_if<LinkStep>(&action)) {
        actions.push_back({{"kind", "link"},
                           {"domain", domainName(link->domain)},
                           {"source", Json::array({link->source.row, link->source.col})},
                           {"direction", directionName(link->direction)},
                           {"elapsed", link->elapsedFromProducerIssue}});
      } else {
        const auto& hold = std::get<VirtualHold>(action);
        actions.push_back({{"kind", "hold"},
                           {"domain", domainName(hold.domain)},
                           {"tile", Json::array({hold.tile.row, hold.tile.col})},
                           {"capture_elapsed", hold.captureElapsed},
                           {"release_elapsed", hold.releaseElapsed}});
      }
    }
    root["plan"] = {{"domain", domainName(plan->domain)},
                    {"required_separation_cycles", plan->requiredSeparationCycles},
                    {"actions", std::move(actions)}};
  }
  for (const auto& diagnostic : diagnostics)
    root["diagnostics"].push_back(
        {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}});
  return root.dump(2) + '\n';
}

RouteSearchResult ModuloRouteSearch::search(const cgra::target::TargetDFG& dfg,
                                            const cgra::TargetModel& target,
                                            const ModuloResourceModel& resources,
                                            const ResourceReservationTable& reservations,
                                            const RouteSearchRequest& request,
                                            const RouteSearchOptions& options) {
  return RouteSearchImpl(dfg, target, resources, reservations, request, options).run();
}

} // namespace cgra::mapping
