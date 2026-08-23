// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Mapping/RouteSearchBudget.h"
#include "cgra/Mapping/RouteSearchResult.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

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
};

class ModuloRouteSearch {
public:
  static RouteSearchResult
  search(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
         const ModuloResourceModel& resources, const ResourceReservationTable& reservations,
         const RouteSearchRequest& request, const RouteSearchOptions& options = {});
};

} // namespace cgra::mapping
