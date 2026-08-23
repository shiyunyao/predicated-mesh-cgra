// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Mapping/RouteSearchStats.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::mapping {

enum class RouteSearchStatus {
  Success,
  NoPath,
  BudgetExceeded,
  InvalidInput,
  UnsupportedEdge,
  TargetContractError,
  InternalError,
};

enum class RouteSearchDiagnosticCode {
  ROUTE_INVALID_TARGET_DFG,
  ROUTE_INVALID_EDGE,
  ROUTE_EDGE_NOT_VALUE_CARRYING,
  ROUTE_PRODUCER_PLACEMENT_MISMATCH,
  ROUTE_CONSUMER_PLACEMENT_MISMATCH,
  ROUTE_SLOT_OUT_OF_RANGE,
  ROUTE_TILE_OUT_OF_RANGE,
  ROUTE_OPERATION_UNSUPPORTED_ON_TILE,
  ROUTE_TARGET_TIMING_MISSING,
  ROUTE_NO_PATH,
  ROUTE_BUDGET_EXCEEDED,
  ROUTE_INTERNAL_SELF_CONFLICT,
  ROUTE_INTERNAL_RECONSTRUCTION_ERROR,
  ROUTE_INTERNAL_VERIFIER_REJECTED,
};

struct RouteSearchDiagnostic {
  RouteSearchDiagnosticCode code = RouteSearchDiagnosticCode::ROUTE_INVALID_EDGE;
  std::string message;
  std::optional<cgra::target::TargetEdgeId> edge;
};

std::string_view toString(RouteSearchStatus status) noexcept;
std::string_view toString(RouteSearchDiagnosticCode code) noexcept;

struct RouteSearchResult {
  RouteSearchStatus status = RouteSearchStatus::InternalError;
  std::optional<TransportPlan> plan;
  RouteSearchStats stats;
  std::vector<RouteSearchDiagnostic> diagnostics;

  bool ok() const noexcept { return status == RouteSearchStatus::Success && plan.has_value(); }
  std::string format() const;
  std::string toJson(cgra::target::TargetEdgeId edge) const;
};

} // namespace cgra::mapping
