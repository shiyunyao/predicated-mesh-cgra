// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/Schedule/MaterializedScheduleSerialization.h"
#include "cgra/Schedule/MaterializedScheduleVerifier.h"
#include "cgra/Schedule/ScheduleMaterializer.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace cgra::schedule {
class MaterializedScheduleTestAccess {
public:
  static MaterializedSchedule make(std::uint32_t ii, std::uint64_t tripCount,
                                   std::uint64_t timeOriginShift, std::uint64_t totalLogicalCycles,
                                   SchedulePhase prologue, RepeatingKernel kernel,
                                   SchedulePhase epilogue) {
    return MaterializedSchedule(ii, tripCount, timeOriginShift, totalLogicalCycles,
                                std::move(prologue), std::move(kernel), std::move(epilogue));
  }
};
} // namespace cgra::schedule

namespace {
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel target() { return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json"); }

cgra::TargetModel readOldTarget() {
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["data_rf"]["same_cycle_read_write_same_address"] = "read_old_then_write_new";
  const auto path = std::filesystem::temp_directory_path() / "cgra-materialize-read-old.json";
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  const auto model = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  return model;
}

cgra::mapping::ModuloMapperOptions options() {
  cgra::mapping::ModuloMapperOptions options;
  options.maxII = 4;
  options.budget.maxNodeCandidateAttempts = 20'000;
  options.budget.maxBacktracks = 10'000;
  options.budget.maxRouteSearchCalls = 20'000;
  options.budget.perRouteBudget.maxStateExpansions = 10'000;
  options.budget.perRouteBudget.maxQueuePushes = 20'000;
  return options;
}

cgra::register_allocation::RFAllocatedMapping mapFixture(const cgra::ir::DFG& generic,
                                                         const cgra::TargetModel& model,
                                                         cgra::target::TargetDFG& dfgOut) {
  const auto legal = cgra::target::TargetLegalizer::legalize(generic, model);
  if (!legal.ok())
    throw std::runtime_error(legal.format());
  dfgOut = *legal.dfg;
  const auto mapped = cgra::mapping::ModuloMapper::map(dfgOut, model, options());
  if (!mapped.ok())
    throw std::runtime_error(mapped.format());
  const auto staged = cgra::schedule::StageScheduler::schedule(dfgOut, model, *mapped.mapping);
  if (!staged.ok())
    throw std::runtime_error(staged.format());
  const auto allocated =
      cgra::register_allocation::RFAllocator::allocate(dfgOut, model, *staged.mapping);
  if (!allocated.ok())
    throw std::runtime_error(allocated.format());
  return *allocated.mapping;
}

void testSimpleAndLargeTripCount(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto mapping = mapFixture(cgra::ir::fixtures::simpleAdd(), model, dfg);
  cgra::schedule::ScheduleMaterializationRequest request;
  request.tripCount = 1'000'000'000;
  const auto result =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, mapping, request);
  if (!result.ok())
    throw std::runtime_error(result.format());
  expect(result.schedule->kernel().repeatCount > 100'000'000,
         "large trip count remains a compact kernel repeat count");
  expect(result.schedule->kernel().body.size() == result.schedule->ii(),
         "materialized kernel has exactly II cycle bundles");
  expect(result.stats.liveOutEvents == 1, "simpleAdd materializes its live-out boundary event");
  expect(cgra::schedule::MaterializedScheduleVerifier::verify(dfg, model, mapping, request,
                                                              *result.schedule)
             .ok(),
         "large materialized schedule independently verifies");
  const auto text = cgra::schedule::MaterializedScheduleSerialization::toJson(*result.schedule);
  const auto roundTrip = cgra::schedule::MaterializedScheduleSerialization::parse(text);
  expect(roundTrip == *result.schedule, "materialized schedule JSON round-trips");
}

void testBudgetAndCorruption(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto mapping = mapFixture(cgra::ir::fixtures::simpleAdd(), model, dfg);
  cgra::schedule::ScheduleMaterializationRequest budgetRequest;
  budgetRequest.tripCount = 4;
  budgetRequest.budget.maxExplicitBoundaryEvents = 0;
  const auto budgetResult =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, mapping, budgetRequest);
  expect(budgetResult.status ==
             cgra::schedule::ScheduleMaterializationStatus::MaterializationBudgetExceeded,
         "explicit one-shot budget is classified separately from semantic failure");

  cgra::schedule::ScheduleMaterializationRequest request;
  request.tripCount = 4;
  const auto result =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, mapping, request);
  if (!result.ok())
    throw std::runtime_error(result.format());
  auto prologue = result.schedule->prologue();
  auto kernel = result.schedule->kernel();
  auto epilogue = result.schedule->epilogue();
  bool removed = false;
  for (auto& cycle : kernel.body) {
    auto it = std::find_if(cycle.events.begin(), cycle.events.end(), [](const auto& event) {
      return event.kind == cgra::schedule::MaterializedEventKind::NodeIssue;
    });
    if (it != cycle.events.end()) {
      cycle.events.erase(it);
      removed = true;
      break;
    }
  }
  expect(removed, "corruption fixture found a kernel node event");
  const auto corrupted = cgra::schedule::MaterializedScheduleTestAccess::make(
      result.schedule->ii(), result.schedule->tripCount(), result.schedule->timeOriginShift(),
      result.schedule->totalLogicalCycles(), std::move(prologue), std::move(kernel),
      std::move(epilogue));
  const auto report =
      cgra::schedule::MaterializedScheduleVerifier::verify(dfg, model, mapping, request, corrupted);
  expect(
      !report.ok() &&
          report.contains(cgra::schedule::MaterializedScheduleVerificationCode::MAT_MISSING_EVENT),
      "independent verifier rejects a dropped kernel event");
}

void testRecurrenceBoundary(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto mapping = mapFixture(cgra::ir::fixtures::recurrence(), model, dfg);
  cgra::schedule::ScheduleMaterializationRequest request;
  request.tripCount = 3;
  const auto result =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, mapping, request);
  if (!result.ok())
    throw std::runtime_error(result.format());
  expect(result.stats.boundarySeedEvents == 1, "distance-one recurrence has one seed event");
  expect(result.stats.liveOutEvents == 0, "recurrence fixture has no live-out event");
  expect(cgra::schedule::MaterializedScheduleVerifier::verify(dfg, model, mapping, request,
                                                              *result.schedule)
             .ok(),
         "recurrence materialization independently verifies");
  bool sawBoundary = false;
  for (const auto& cycle : result.schedule->prologue().cycles)
    for (const auto& event : cycle.events)
      sawBoundary |= event.kind == cgra::schedule::MaterializedEventKind::BoundaryValueInject;
  expect(sawBoundary, "recurrence boundary seed is materialized in the finite prefix");
}

void testSoftwareRotationSuperkernel(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto legal = cgra::target::TargetLegalizer::legalize(cgra::ir::fixtures::recurrence(), model);
  if (!legal.ok())
    throw std::runtime_error(legal.format());
  dfg = *legal.dfg;
  auto mapperOptions = options();
  mapperOptions.rfPortAware.enabled = false;
  const auto mapped = cgra::mapping::ModuloMapper::map(dfg, model, mapperOptions);
  if (!mapped.ok())
    throw std::runtime_error(mapped.format());
  const auto staged = cgra::schedule::StageScheduler::schedule(dfg, model, *mapped.mapping);
  if (!staged.ok())
    throw std::runtime_error(staged.format());
  cgra::register_allocation::RFAllocationOptions rfOptions;
  rfOptions.enableSoftwareRotation = true;
  const auto allocated = cgra::register_allocation::RFAllocator::allocate(
      dfg, model, *staged.mapping, rfOptions);
  if (!allocated.ok())
    throw std::runtime_error(allocated.format());
  expect(allocated.mapping->rotationPeriodIterations() == 2,
         "recurrence rotation uses a two-iteration control period");
  for (const auto tripCount : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 7ULL}) {
    cgra::schedule::ScheduleMaterializationRequest request;
    request.tripCount = tripCount;
    const auto materialized = cgra::schedule::ScheduleMaterializer::materialize(
        dfg, model, *allocated.mapping, request);
    if (!materialized.ok())
      throw std::runtime_error("rotating recurrence materialization: " + materialized.format());
    expect(materialized.schedule->controlPeriodCycles() ==
               2 * materialized.schedule->logicalII(),
           "materialized schedule records the expanded control period");
    expect(cgra::schedule::MaterializedScheduleVerifier::verify(
               dfg, model, *allocated.mapping, request, *materialized.schedule)
               .ok(),
           "rotating recurrence schedule passes independent verification");
  }
}

void testInvalidTripCount(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto mapping = mapFixture(cgra::ir::fixtures::simpleAdd(), model, dfg);
  cgra::schedule::ScheduleMaterializationRequest request;
  const auto result =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, mapping, request);
  expect(result.status == cgra::schedule::ScheduleMaterializationStatus::InvalidTripCount,
         "zero trip count is rejected by the V0 contract");
}
} // namespace

int main() {
  try {
    const auto canonical = target();
    testSimpleAndLargeTripCount(canonical);
    testRecurrenceBoundary(readOldTarget());
    testSoftwareRotationSuperkernel(canonical);
    testInvalidTripCount(canonical);
    testBudgetAndCorruption(canonical);
    std::cout << "schedule materialization tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "schedule materialization tests failed: " << error.what() << '\n';
    return 1;
  }
}
