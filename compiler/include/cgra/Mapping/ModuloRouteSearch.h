// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Mapping/RouteSearchBudget.h"
#include "cgra/Mapping/RouteSearchResult.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace cgra::mapping {

struct RouteSearchRequest {
  cgra::target::TargetEdgeId edge = 0;
  NodePlacement producer;
  NodePlacement consumer;
};

enum class RouteTieBreakPolicy {
  MinSeparationThenHoldsThenHops,
};

struct RouteSearchOptions {
  RouteSearchBudget budget;
  bool allowVirtualHold = true;
  RouteTieBreakPolicy tieBreak = RouteTieBreakPolicy::MinSeparationThenHoldsThenHops;
  struct ActionBan {
    NetworkDomain domain = NetworkDomain::Data;
    TileCoord tile;
    std::uint32_t elapsed = 0;
    std::optional<Direction> direction;
    friend bool operator==(const ActionBan&, const ActionBan&) = default;
  };
  std::vector<ActionBan> bannedActions;
};

struct RouteAlternativeRequest {
  RouteSearchRequest base;
  std::uint32_t maxAlternatives = 1;
};

struct RouteAlternativeResult {
  RouteSearchStatus status = RouteSearchStatus::InternalError;
  std::vector<TransportPlan> plans;
  RouteSearchStats stats;
  std::vector<RouteSearchDiagnostic> diagnostics;
};

class ModuloRouteSearch {
public:
  static RouteSearchResult
  search(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
         const ModuloResourceModel& resources, const ResourceReservationTable& reservations,
         const RouteSearchRequest& request, const RouteSearchOptions& options = {});

  static RouteAlternativeResult searchAlternatives(
      const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
      const ModuloResourceModel& resources, const ResourceReservationTable& reservations,
      const RouteAlternativeRequest& request, const RouteSearchOptions& options = {});
};

} // namespace cgra::mapping
