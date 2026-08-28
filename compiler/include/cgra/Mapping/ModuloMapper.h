// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/CompleteMappingChecker.h"
#include "cgra/Mapping/ModuloMapperBudget.h"
#include "cgra/Mapping/ModuloMapperResult.h"
#include "cgra/Mapping/ModuloRouteSearch.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::mapping {

enum class MappingObjective {
  OptimizeII,
  FindAnyFeasible,
};

struct FeasibilityFallbackOptions {
  bool enabled = true;
  std::uint32_t lowIIWindow = 4;
  std::uint32_t maxSafeII = 0;
  std::uint64_t maxLocalRepairs = 100000;
  bool deterministic = true;
  std::uint64_t seed = 0;
};

struct ModuloMapperOptions {
  std::uint32_t maxII = 0;
  std::uint32_t minII = 0;
  ModuloMapperBudget budget;
  RouteSearchOptions routeOptions;
  CompleteMappingChecker completeMappingChecker;
  MappingObjective objective = MappingObjective::OptimizeII;
  FeasibilityFallbackOptions feasibilityFallback;
};

class ModuloMapper {
public:
  static ModuloMapperResult map(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                                const ModuloMapperOptions& options = {});
};

} // namespace cgra::mapping
