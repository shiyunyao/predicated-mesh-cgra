// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapperResult.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloRouteSearch.h"
#include "cgra/Mapping/ModuloMapperBudget.h"
#include "cgra/Mapping/CompleteMappingChecker.h"
#include "cgra/Mapping/RFPortReservation.h"

#include <cstdint>
#include <map>

namespace cgra::mapping {

// Absolute-time placement is deliberately kept separate from the modulo
// mapping representation. It makes the coverage fallback deterministic while
// the final candidate still goes through the normal modulo verifiers.
struct AbsoluteNodePlacement {
  cgra::target::TargetNodeId node = 0;
  TileCoord tile;
  std::uint64_t issueCycle = 0;
};

struct AbsoluteTransport {
  cgra::target::TargetEdgeId edge = 0;
  std::uint64_t producerCycle = 0;
  std::uint64_t consumerCycle = 0;
  TransportPlan transport;
};

struct ConstructiveSchedule {
  std::uint64_t period = 0;
  std::map<cgra::target::TargetNodeId, AbsoluteNodePlacement> placements;
  std::map<cgra::target::TargetEdgeId, AbsoluteTransport> transports;
};

struct ConstructiveModuloMapperOptions {
  std::uint32_t minII = 1;
  std::uint32_t maxSafeII = 0;
  std::uint64_t maxLocalRepairs = 100000;
  std::uint64_t seed = 0;
  CompleteMappingChecker completeMappingChecker;
  ModuloMapperBudget budget;
  RouteSearchOptions routeOptions;
  RFPortAwareMappingOptions rfPortAware;
};

struct ConstructiveModuloMapperStats {
  std::uint64_t scheduleAttempts = 0;
  std::uint64_t placementRepairs = 0;
  std::uint64_t routeRepairs = 0;
  std::uint64_t stageRepairs = 0;
  std::uint64_t rfRepairs = 0;
  std::uint64_t periodGrowth = 0;
  std::uint64_t successfulPlacements = 0;
  std::uint64_t routeSearchCalls = 0;
  std::uint64_t routeSuccesses = 0;
  std::uint64_t routeNoPaths = 0;
  std::uint64_t routeBudgetExceeded = 0;
  std::uint64_t routeStateExpansions = 0;
  std::uint64_t rfPortMatchCalls = 0;
  std::uint64_t rfPortMatchFailures = 0;
  std::uint64_t rfReadPortEarlyRejects = 0;
  std::uint64_t rfWritePortEarlyRejects = 0;
  std::uint64_t rfWriteSourceEarlyRejects = 0;
  std::uint64_t rfPortEventsCommitted = 0;
  std::uint64_t rfPortRollbackCount = 0;
  std::uint64_t lateReadPortConflicts = 0;
  std::uint64_t lateWritePortConflicts = 0;
};

ModuloMapperResult mapConstructively(const target::TargetDFG& dfg, const TargetModel& target,
                                     const ConstructiveModuloMapperOptions& options = {});

} // namespace cgra::mapping
