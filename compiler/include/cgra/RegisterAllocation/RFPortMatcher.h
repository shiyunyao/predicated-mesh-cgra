// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFPortEvent.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <span>
#include <vector>

namespace cgra::register_allocation {

struct RFPortMatchAssignment {
  std::uint64_t eventId = 0;
  std::uint32_t port = 0;
};

enum class RFPortMatchStatus {
  Success,
  CapacityExceeded,
  SourceCompatibilityFailure,
  InvalidTargetContract,
};

struct RFPortMatchResult {
  RFPortMatchStatus status = RFPortMatchStatus::Success;
  std::vector<RFPortMatchAssignment> assignments;
  std::vector<std::uint64_t> conflictingEvents;

  bool ok() const noexcept { return status == RFPortMatchStatus::Success; }
};

RFPortMatchResult matchRFPorts(const cgra::RegisterFileDesc& bank,
                               std::span<const RFPortEvent> events);

} // namespace cgra::register_allocation
