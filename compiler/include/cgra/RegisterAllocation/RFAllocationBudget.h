// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace cgra::register_allocation {

struct RFAllocationBudget {
  std::uint64_t maxColoringDecisions = 100000;
  std::uint64_t maxColoringBacktracks = 100000;
};

struct RFAllocationOptions {
  RFAllocationBudget budget;
};

} // namespace cgra::register_allocation
