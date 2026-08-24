// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>

namespace cgra::mapping {

struct RouteSearchStats {
  std::uint64_t stateExpansions = 0;
  std::uint64_t queuePushes = 0;
  std::uint64_t linkTransitionsConsidered = 0;
  std::uint64_t holdTransitionsConsidered = 0;
  std::uint64_t blockedLinks = 0;
  std::uint64_t invalidBorderLinks = 0;
  std::uint32_t maxQueueSize = 0;
  std::optional<std::uint32_t> resultSeparation;
  std::uint32_t resultHopCount = 0;
  std::uint32_t resultHoldCycles = 0;
};

} // namespace cgra::mapping
