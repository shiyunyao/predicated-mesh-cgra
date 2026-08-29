// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RotationFactorAnalysis.h"

#include <stdexcept>
#include <vector>

namespace {

void expect(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

cgra::register_allocation::StorageSegment segment(std::uint32_t id, std::uint64_t write,
                                                  std::uint64_t read) {
  cgra::register_allocation::StorageSegment value;
  value.id = id;
  value.writeTime = write;
  value.readTime = read;
  return value;
}

void forbiddenBoundaryNeedsTwoPhases() {
  const std::vector segments = {segment(3, 0, 4)};
  const auto plan = cgra::register_allocation::analyzeRotationFactors(
      segments, 4, cgra::SameAddressReadWritePolicy::Forbidden);
  expect(plan.ok(), "rotation analysis should succeed");
  expect(plan.segments.front().minimumPhaseCount == 2, "duration == II needs two phases");
  expect(plan.rotationPeriodIterations == 2, "rotation period should be two iterations");
  expect(plan.controlPeriodCycles == 8, "control period should be II times phase period");
}

void readOldThenWriteCanReuseAtBoundary() {
  const std::vector segments = {segment(3, 0, 4)};
  const auto plan = cgra::register_allocation::analyzeRotationFactors(
      segments, 4, cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew);
  expect(plan.ok(), "read-old policy should succeed");
  expect(plan.segments.front().minimumPhaseCount == 1, "boundary reuse should need one phase");
}

void lcmIsDeterministic() {
  const std::vector segments = {segment(1, 0, 4), segment(2, 0, 8)};
  const auto plan = cgra::register_allocation::analyzeRotationFactors(
      segments, 4, cgra::SameAddressReadWritePolicy::Forbidden);
  expect(plan.ok(), "LCM analysis should succeed");
  expect(plan.rotationPeriodIterations == 6, "phase factors 2 and 3 must have LCM six");
  expect(plan.controlPeriodCycles == 24, "control period must include the LCM");
}

void durationEqualToTwoPeriodsNeedsThreePhases() {
  const std::vector segments = {segment(9, 0, 8)};
  const auto plan = cgra::register_allocation::analyzeRotationFactors(
      segments, 4, cgra::SameAddressReadWritePolicy::Forbidden);
  expect(plan.ok(), "three-phase rotation analysis should succeed");
  expect(plan.segments.front().minimumPhaseCount == 3,
         "duration equal to two II periods needs three phases under forbidden reuse");
  expect(plan.rotationPeriodIterations == 3,
         "single three-phase family has a three-iteration control period");
  expect(plan.controlPeriodCycles == 12,
         "three-phase control period is II times three");
}

void controlMemoryCapIsEnforced() {
  const std::vector segments = {segment(1, 0, 4), segment(2, 0, 8)};
  const auto plan = cgra::register_allocation::analyzeRotationFactors(
      segments, 4, cgra::SameAddressReadWritePolicy::Forbidden, 16);
  expect(plan.status == cgra::register_allocation::RotationAnalysisStatus::
                              RotationPeriodExceedsControlMemory,
         "control memory cap must reject an oversized superperiod");
}

} // namespace

int main() {
  forbiddenBoundaryNeedsTwoPhases();
  readOldThenWriteCanReuseAtBoundary();
  lcmIsDeterministic();
  durationEqualToTwoPeriodsNeedsThreePhases();
  controlMemoryCapIsEnforced();
  return 0;
}
