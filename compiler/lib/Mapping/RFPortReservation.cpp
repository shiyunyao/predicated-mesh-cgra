// SPDX-License-Identifier: MIT
#include "cgra/Mapping/RFPortReservation.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace cgra::mapping {

RFPortReservationResult RFPortReservationTable::tryReserve(
    std::span<const cgra::register_allocation::RFPortEvent> events) {
  RFPortReservationResult result;
  std::map<RFPortReservationKey, std::vector<cgra::register_allocation::RFPortEvent>> additions;
  for (const auto& event : events) {
    const bool writes = event.kind != cgra::register_allocation::RFPortEventKind::PeriodicRead;
    additions[{event.tile, event.domain, event.slot, writes}].push_back(event);
  }

  RFPortReservationDelta delta;
  for (const auto& [key, newEvents] : additions) {
    const auto* bank = target_.registerBank(key.domain, key.tile.row, key.tile.col);
    if (!bank) {
      result.status = RFPortReservationStatus::MissingRegisterBank;
      result.failure = RFPortReservationFailure{result.status, key, {}};
      undo(delta);
      return result;
    }
    const auto found = entries_.find(key);
    RFPortReservationSnapshot snapshot;
    snapshot.key = key;
    if (found != entries_.end()) {
      snapshot.events = found->second.events;
      snapshot.assignments = found->second.assignments;
    }
    delta.previous.push_back(snapshot);

    std::vector<cgra::register_allocation::RFPortEvent> merged = snapshot.events;
    merged.insert(merged.end(), newEvents.begin(), newEvents.end());
    const auto match = cgra::register_allocation::matchRFPorts(*bank, merged);
    if (!match.ok()) {
      // Restore all keys touched by this transaction, including keys matched
      // successfully before the failing key was visited.
      undo(delta);
      result.status = key.writes
                          ? (match.status == cgra::register_allocation::RFPortMatchStatus::CapacityExceeded
                                 ? RFPortReservationStatus::WriteCapacityExceeded
                                 : match.status == cgra::register_allocation::RFPortMatchStatus::SourceCompatibilityFailure
                                     ? RFPortReservationStatus::WriteSourceCompatibilityFailure
                                     : RFPortReservationStatus::InvalidEventSet)
                          : (match.status == cgra::register_allocation::RFPortMatchStatus::CapacityExceeded
                                 ? RFPortReservationStatus::ReadCapacityExceeded
                                 : RFPortReservationStatus::InvalidEventSet);
      result.failure = RFPortReservationFailure{result.status, key, match.conflictingEvents};
      return result;
    }
    entries_[key] = {std::move(merged), match.assignments};
  }
  result.status = RFPortReservationStatus::Success;
  result.delta = std::move(delta);
  return result;
}

void RFPortReservationTable::undo(const RFPortReservationDelta& delta) {
  for (const auto& snapshot : delta.previous) {
    if (snapshot.events.empty() && snapshot.assignments.empty())
      entries_.erase(snapshot.key);
    else
      entries_[snapshot.key] = {snapshot.events, snapshot.assignments};
  }
}

std::vector<cgra::register_allocation::RFPortEvent> RFPortReservationTable::events() const {
  std::vector<cgra::register_allocation::RFPortEvent> result;
  for (const auto& [key, entry] : entries_)
    result.insert(result.end(), entry.events.begin(), entry.events.end());
  return result;
}

std::span<const cgra::register_allocation::RFPortMatchAssignment>
RFPortReservationTable::assignments(const RFPortReservationKey& key) const {
  const auto found = entries_.find(key);
  if (found == entries_.end())
    return {};
  return found->second.assignments;
}

void RFPortReservationTable::clear() noexcept { entries_.clear(); }

} // namespace cgra::mapping
