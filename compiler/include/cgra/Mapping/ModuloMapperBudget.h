// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/RouteSearchBudget.h"

#include <cstdint>

namespace cgra::mapping {

struct ModuloMapperBudget {
  std::uint64_t maxNodeCandidateAttempts = 100000;
  std::uint64_t maxBacktracks = 50000;
  std::uint64_t maxRouteSearchCalls = 100000;
  RouteSearchBudget perRouteBudget;
};

} // namespace cgra::mapping
