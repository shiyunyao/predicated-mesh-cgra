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
  bool enableSoftwareRotation = false;
  std::uint32_t maxRotationFactor = 0;
  std::uint32_t maxControlPeriodCycles = 0;
};

} // namespace cgra::register_allocation
