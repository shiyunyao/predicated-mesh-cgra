// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/TileCoord.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <string>

namespace cgra::register_allocation {

// A normalized RF access independent of the allocator that discovered it.
// The mapper uses these events for early pruning; the allocator and verifier
// use the same representation for the final authoritative check.
enum class RFPortEventKind {
  PeriodicRead,
  PeriodicWrite,
  BoundaryWrite,
};

struct RFPortEvent {
  std::uint64_t id = 0;
  RFPortEventKind kind = RFPortEventKind::PeriodicRead;
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankDomain domain = cgra::RegisterBankDomain::Data;
  std::uint32_t slot = 0;
  std::optional<std::string> writeSource;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<std::uint32_t> segment;
  std::optional<std::uint32_t> transportActionIndex;
};

} // namespace cgra::register_allocation
