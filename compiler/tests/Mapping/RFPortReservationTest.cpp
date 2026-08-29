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

cgra::register_allocation::RFPortEvent write(std::uint64_t id, const char* source,
                                              std::uint32_t slot = 0) {
  auto event = read(id, slot);
  event.kind = cgra::register_allocation::RFPortEventKind::PeriodicWrite;
  event.writeSource = source;
  return event;
}

void transactionRollsBack() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  cgra::mapping::RFPortReservationTable table(target);
  const std::vector first = {read(1), read(2)};
  const auto delta = table.tryReserve(first);
  expect(delta.ok(), "initial capacity should be available");
  const std::vector conflicting = {read(3)};
  const auto failure = table.tryReserve(conflicting);
  expect(!failure.ok(), "third read should be rejected");
  expect(failure.status == cgra::mapping::RFPortReservationStatus::ReadCapacityExceeded,
         "read capacity failure should be classified");
  expect(table.events().size() == 2U, "failed transaction must not leak its event");
  table.undo(*delta.delta);
  expect(table.events().empty(), "undo must restore the previous table");
}

void failuresAreStructured() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  cgra::mapping::RFPortReservationTable table(target);

  const std::vector tooManyReads = {read(10), read(11), read(12)};
  const auto readFailure = table.tryReserve(tooManyReads);
  expect(readFailure.status == cgra::mapping::RFPortReservationStatus::ReadCapacityExceeded,
         "read overflow should have a read-specific status");
  expect(readFailure.failure.has_value() && !readFailure.failure->conflictingEvents.empty(),
         "read overflow should report conflicting events");

  const std::vector tooManyWrites = {write(20, "FU_DATA_RESULT"),
                                     write(21, "FU_DATA_RESULT"),
                                     write(22, "FU_DATA_RESULT")};
  const auto writeFailure = table.tryReserve(tooManyWrites);
  expect(writeFailure.status == cgra::mapping::RFPortReservationStatus::WriteCapacityExceeded,
         "write overflow should have a write-specific status");

  const std::vector sourceFailure = {write(30, "FU_DATA_RESULT"),
                                     write(31, "FU_DATA_RESULT")};
  // Two FU results can only use W0 on the v2 contract, so this is a
  // source-compatibility failure even though the bank has two write ports.
  const auto sourceResult = table.tryReserve(sourceFailure);
  expect(sourceResult.status ==
             cgra::mapping::RFPortReservationStatus::WriteSourceCompatibilityFailure,
         "source-incompatible writes should be classified separately");

  auto missingSource = write(40, "FU_DATA_RESULT");
  missingSource.writeSource.reset();
  const std::vector missingSourceEvents = {missingSource};
  const auto missingSourceResult = table.tryReserve(missingSourceEvents);
  expect(missingSourceResult.status ==
             cgra::mapping::RFPortReservationStatus::WriteSourceCompatibilityFailure,
         "missing write source should be classified separately");
  expect(table.events().empty(), "failed transactions must not leak events");
}

} // namespace

int main() {
  transactionRollsBack();
  failuresAreStructured();
  return 0;
}
