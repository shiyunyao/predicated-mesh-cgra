// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace cgra::mapping {

struct RouteSearchBudget {
  // Zero is an intentional budget: the search returns BudgetExceeded without expanding.
  std::uint64_t maxStateExpansions = 100000;
  std::uint64_t maxQueuePushes = 200000;
};

} // namespace cgra::mapping
