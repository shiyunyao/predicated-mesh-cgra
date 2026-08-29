// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/PhaseRegisterColoring.h"

#include <filesystem>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::register_allocation::StorageSegment segment(std::uint32_t id,
                                                  std::uint64_t write,
                                                  std::uint64_t read) {
  return {id, id, {0, 0}, cgra::RegisterBankDomain::Data, write, read, {}};
}

void testGlobalPhaseColoring() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  const std::vector segments = {segment(0, 0, 8), segment(1, 2, 5)};
  cgra::register_allocation::RotationPlan plan;
  plan.segments = {{0, 2, 8, 4}, {1, 1, 3, 4}};
  const auto colored = cgra::register_allocation::colorPhaseRegisters(
      segments, plan, target, cgra::register_allocation::RFAllocationBudget{});
  expect(colored.status ==
             cgra::register_allocation::PhaseRegisterColoringResult::Status::Success,
         "phase graph should be colorable on the v2 bank");
  expect(colored.colors.size() == 3U, "all phase vertices should be colored");
  expect(colored.colors.at({0, 0}) != colored.colors.at({0, 1}),
         "phases of one family must use distinct registers");
}

void testDepthFailure() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  const std::vector segments = {segment(0, 0, 8)};
  cgra::register_allocation::RotationPlan plan;
  plan.segments = {{0, 17, 8, 4}};
  const auto colored = cgra::register_allocation::colorPhaseRegisters(
      segments, plan, target, cgra::register_allocation::RFAllocationBudget{});
  expect(colored.status ==
             cgra::register_allocation::PhaseRegisterColoringResult::Status::RegisterDepthInfeasible,
         "phase count above bank depth must fail");
}

} // namespace

int main() {
  testGlobalPhaseColoring();
  testDepthFailure();
  return 0;
}
