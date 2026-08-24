// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapperBudget.h"
#include "cgra/Mapping/ModuloMapperResult.h"
#include "cgra/Mapping/ModuloRouteSearch.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

namespace cgra::mapping {

struct ModuloMapperOptions {
  std::uint32_t maxII = 0;
  std::uint32_t minII = 0;
  ModuloMapperBudget budget;
  RouteSearchOptions routeOptions;
};

class ModuloMapper {
public:
  static ModuloMapperResult map(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                                const ModuloMapperOptions& options = {});
};

} // namespace cgra::mapping
