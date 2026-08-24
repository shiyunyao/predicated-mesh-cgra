// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/RegisterAllocation/PeriodicLifetime.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace cgra::register_allocation;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel target() { return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json"); }

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& model) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, model);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

cgra::mapping::ModuloMapperOptions mapperOptions() {
  cgra::mapping::ModuloMapperOptions options;
  options.maxII = 4;
  options.budget.maxNodeCandidateAttempts = 20000;
  options.budget.maxBacktracks = 10000;
  options.budget.maxRouteSearchCalls = 20000;
  options.budget.perRouteBudget.maxStateExpansions = 10000;
  options.budget.perRouteBudget.maxQueuePushes = 20000;
  return options;
}

std::pair<cgra::target::TargetDFG, cgra::schedule::StagedMapping>
mapAndStage(const cgra::ir::DFG& generic, const cgra::TargetModel& model) {
  const auto dfg = legalize(generic, model);
  const auto mapped = cgra::mapping::ModuloMapper::map(dfg, model, mapperOptions());
  if (!mapped.ok())
    throw std::runtime_error(mapped.format());
  const auto staged = cgra::schedule::StageScheduler::schedule(dfg, model, *mapped.mapping);
  if (!staged.ok())
    throw std::runtime_error(staged.format());
  return {dfg, *staged.mapping};
}

void testCanonicalNoStorage(const cgra::TargetModel& model) {
  const auto [dfg, staged] = mapAndStage(cgra::ir::fixtures::simpleAdd(), model);
  const auto requirements = StorageRequirementAnalysis::analyze(dfg, model, staged);
  expect(requirements.ok(), "simple add storage analysis succeeds");
  expect(requirements.requirements->segments().empty(), "direct route has no RF storage");
  const auto allocation = RFAllocator::allocate(dfg, model, staged);
  expect(allocation.ok(), "simple add RF allocation succeeds");
  expect(RFAllocationVerifier::verify(dfg, model, *allocation.mapping).ok(),
         "simple add allocation passes independent verifier");
}

void testVirtualHoldAndRecurrence(const cgra::TargetModel& model) {
  const auto [dfg, staged] = mapAndStage(cgra::ir::fixtures::recurrence(), model);
  const auto requirements = StorageRequirementAnalysis::analyze(dfg, model, staged);
  expect(requirements.ok() && !requirements.requirements->segments().empty(),
         "recurrence derives explicit storage");
  const auto allocation = RFAllocator::allocate(dfg, model, staged);
  expect(allocation.status == RFAllocationStatus::FixedRegisterSelfOverlap,
         "canonical forbidden same-address policy rejects periodic recurrence storage");

  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["data_rf"]["same_cycle_read_write_same_address"] = "read_old_then_write_new";
  const auto path = std::filesystem::temp_directory_path() / "cgra-rf-read-old.json";
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  const auto readOldTarget = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  const auto [readOldDfg, readOldStaged] =
      mapAndStage(cgra::ir::fixtures::recurrence(), readOldTarget);
  const auto readOldAllocation = RFAllocator::allocate(readOldDfg, readOldTarget, readOldStaged);
  if (!readOldAllocation.ok())
    throw std::runtime_error("read-old recurrence allocation: " + readOldAllocation.format());
  expect(readOldAllocation.mapping->registerForVirtualHold(1, 0).has_value(),
         "VirtualHold provenance resolves to physical RF");
  const auto roundTrip = RFAllocatedMappingSerialization::parse(
      RFAllocatedMappingSerialization::toJson(*readOldAllocation.mapping));
  expect(roundTrip == *readOldAllocation.mapping,
         "RF allocation debug serialization preserves the complete allocation");
}

void testPeriodicHelpers() {
  StorageSegment shortSegment{0, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 4, {}};
  expect(!fixedRegisterSelfOverlaps(shortSegment, 4), "short lifetime is self-compatible");
  StorageSegment longSegment{1, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 8, {}};
  expect(fixedRegisterSelfOverlaps(longSegment, 4), "long lifetime self-overlaps");
  StorageSegment boundary{2, 0, {0, 0}, cgra::RegisterBankDomain::Data, 0, 2, {}};
  StorageSegment wrapped{3, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 4, {}};
  expect(periodicLifetimesConflict(boundary, wrapped, 4),
         "periodic modulo-boundary conflict is detected");
  expect(!periodicLifetimesConflict(boundary, wrapped, 4,
                                    cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew),
         "read-old/write-new allows boundary-only reuse");
}

void testTargetRFMutation(const cgra::TargetModel& canonical) {
  (void)canonical;
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["data_rf"]["read_ports"] = 1;
  json["data_rf"]["write_ports"] = 1;
  json["data_rf"]["write_ports_detail"].erase("W1");
  const auto path = std::filesystem::temp_directory_path() / "cgra-rf-mutation.json";
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  const auto mutated = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  expect(mutated.dataRF().readPorts == 1 && mutated.dataRF().writePorts == 1,
         "RF port mutation is target-driven");
}
} // namespace

int main() {
  try {
    const auto model = target();
    testCanonicalNoStorage(model);
    testVirtualHoldAndRecurrence(model);
    testPeriodicHelpers();
    testTargetRFMutation(model);
    std::cout << "RF allocation tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "RF allocation tests failed: " << error.what() << '\n';
    return 1;
  }
}
