// SPDX-License-Identifier: MIT
#include "cgra/Mapping/RFPortReservation.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {

void expect(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

cgra::register_allocation::RFPortEvent read(std::uint64_t id, std::uint32_t slot = 0) {
  cgra::register_allocation::RFPortEvent event;
  event.id = id;
  event.kind = cgra::register_allocation::RFPortEventKind::PeriodicRead;
  event.tile = {0, 0};
  event.domain = cgra::RegisterBankDomain::Data;
  event.slot = slot;
  return event;
}

void transactionRollsBack() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  cgra::mapping::RFPortReservationTable table(target);
  const std::vector first = {read(1), read(2)};
  const auto delta = table.tryReserve(first);
  expect(delta.has_value(), "initial capacity should be available");
  const std::vector conflicting = {read(3)};
  expect(!table.tryReserve(conflicting), "third read should be rejected");
  expect(table.events().size() == 2U, "failed transaction must not leak its event");
  table.undo(*delta);
  expect(table.events().empty(), "undo must restore the previous table");
}

} // namespace

int main() {
  transactionRollsBack();
  return 0;
}
