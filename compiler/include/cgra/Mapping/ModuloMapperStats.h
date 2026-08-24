// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace cgra::mapping {

struct ModuloMapperStats {
  std::uint32_t startingMII = 0;
  std::uint32_t finalII = 0;
  std::uint64_t iiAttempts = 0;
  std::uint64_t nodeCandidateAttempts = 0;
  std::uint64_t successfulPlacements = 0;
  std::uint64_t rejectedPlacements = 0;
  std::uint64_t routeSearchCalls = 0;
  std::uint64_t routeSuccesses = 0;
  std::uint64_t routeNoPaths = 0;
  std::uint64_t routeBudgetExceeded = 0;
  std::uint64_t backtracks = 0;
  std::uint64_t maxSearchDepth = 0;
  std::uint64_t totalRouteStateExpansions = 0;
};

} // namespace cgra::mapping
