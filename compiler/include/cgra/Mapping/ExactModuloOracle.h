// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>

namespace cgra::mapping {

enum class ExactOracleStatus {
  Feasible,
  Infeasible,
  UnsupportedOracleSize,
  InvalidInput,
};

struct ExactOracleOptions {
  std::uint32_t maxNodes = 5;
  std::uint32_t maxEdges = 5;
  std::uint32_t maxTiles = 6;
  std::uint32_t maxII = 3;
};

struct ExactOracleResult {
  ExactOracleStatus status = ExactOracleStatus::InvalidInput;
  std::optional<ModuloMapping> mapping;
};

// Deliberately tiny, independent oracle used by CI-100. It exhaustively
// enumerates operation placements and finite modulo routes for small graphs.
// It must remain independent from the production mapper and route search.
class ExactModuloOracle {
public:
  static ExactOracleResult solve(const cgra::target::TargetDFG& dfg,
                                 const cgra::TargetModel& target, std::uint32_t ii,
                                 ExactOracleOptions options = {});
};

} // namespace cgra::mapping
