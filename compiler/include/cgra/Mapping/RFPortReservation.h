// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFPortMatcher.h"

#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace cgra::mapping {

struct RFPortReservationKey {
  TileCoord tile;
  cgra::RegisterBankDomain domain = cgra::RegisterBankDomain::Data;
  std::uint32_t slot = 0;
  bool writes = false;

  friend bool operator<(const RFPortReservationKey& lhs, const RFPortReservationKey& rhs) {
    return std::tie(lhs.tile.row, lhs.tile.col, lhs.domain, lhs.slot, lhs.writes) <
           std::tie(rhs.tile.row, rhs.tile.col, rhs.domain, rhs.slot, rhs.writes);
  }
};

struct RFPortReservationSnapshot {
  RFPortReservationKey key;
  std::vector<cgra::register_allocation::RFPortEvent> events;
  std::vector<cgra::register_allocation::RFPortMatchAssignment> assignments;
};

struct RFPortReservationDelta {
  std::vector<RFPortReservationSnapshot> previous;
};

class RFPortReservationTable {
public:
  explicit RFPortReservationTable(const cgra::TargetModel& target) : target_(target) {}

  std::optional<RFPortReservationDelta>
  tryReserve(std::span<const cgra::register_allocation::RFPortEvent> events);

  void undo(const RFPortReservationDelta& delta);
  std::vector<cgra::register_allocation::RFPortEvent> events() const;
  std::span<const cgra::register_allocation::RFPortMatchAssignment> assignments(
      const RFPortReservationKey& key) const;
  void clear() noexcept;

private:
  struct Entry {
    std::vector<cgra::register_allocation::RFPortEvent> events;
    std::vector<cgra::register_allocation::RFPortMatchAssignment> assignments;
  };

  const cgra::TargetModel& target_;
  std::map<RFPortReservationKey, Entry> entries_;
};

} // namespace cgra::mapping
