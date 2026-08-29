// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFPortMatcher.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

cgra::RegisterFileDesc dataBank() {
  cgra::RegisterFileDesc bank;
  bank.id = "D";
  bank.domain = cgra::RegisterBankDomain::Data;
  bank.readPorts = 2;
  bank.writePorts = 2;
  bank.writePortSources["W0"] = {"FU_DATA_RESULT"};
  bank.writePortSources["W1"] = {"NORTH_DATA_IN", "LSU_LOAD_DATA", "CONST_DATA"};
  return bank;
}

cgra::register_allocation::RFPortEvent read(std::uint64_t id) {
  cgra::register_allocation::RFPortEvent event;
  event.id = id;
  event.kind = cgra::register_allocation::RFPortEventKind::PeriodicRead;
  return event;
}

cgra::register_allocation::RFPortEvent write(std::uint64_t id, std::string source) {
  cgra::register_allocation::RFPortEvent event;
  event.id = id;
  event.kind = cgra::register_allocation::RFPortEventKind::PeriodicWrite;
  event.writeSource = std::move(source);
  return event;
}

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void matchesReadsUpToCapacity() {
  const auto bank = dataBank();
  const std::vector events = {read(1), read(2)};
  const auto result = cgra::register_allocation::matchRFPorts(bank, events);
  expect(result.ok(), "two reads should match");
  expect(result.assignments.size() == 2U, "two read assignments expected");
}

void rejectsReadOverflow() {
  const auto bank = dataBank();
  const std::vector events = {read(1), read(2), read(3)};
  const auto result = cgra::register_allocation::matchRFPorts(bank, events);
  expect(result.status == cgra::register_allocation::RFPortMatchStatus::CapacityExceeded,
         "read overflow should be rejected");
}

void matchesAsymmetricWriteSources() {
  const auto bank = dataBank();
  const std::vector events = {write(1, "FU_DATA_RESULT"), write(2, "NORTH_DATA_IN")};
  const auto result = cgra::register_allocation::matchRFPorts(bank, events);
  expect(result.ok(), "asymmetric sources should match");
  expect(result.assignments.size() == 2U, "two write assignments expected");
  expect(result.assignments[0].port == 0U, "FU result must use W0");
  expect(result.assignments[1].port == 1U, "network input must use W1");
}

void rejectsTwoSourcesRestrictedToOnePort() {
  const auto bank = dataBank();
  const std::vector events = {write(1, "FU_DATA_RESULT"), write(2, "FU_DATA_RESULT")};
  const auto result = cgra::register_allocation::matchRFPorts(bank, events);
  expect(result.status == cgra::register_allocation::RFPortMatchStatus::SourceCompatibilityFailure,
         "two FU results should not share W0");
}

void readAndWriteDirectionsAreMatchedIndependently() {
  const auto bank = dataBank();
  const std::vector reads = {read(1), read(2)};
  const std::vector writes = {write(3, "FU_DATA_RESULT"), write(4, "NORTH_DATA_IN")};
  expect(cgra::register_allocation::matchRFPorts(bank, reads).ok(), "reads should match");
  expect(cgra::register_allocation::matchRFPorts(bank, writes).ok(), "writes should match");
}

} // namespace

int main() {
  matchesReadsUpToCapacity();
  rejectsReadOverflow();
  matchesAsymmetricWriteSources();
  rejectsTwoSourcesRestrictedToOnePort();
  readAndWriteDirectionsAreMatchedIndependently();
  return 0;
}
