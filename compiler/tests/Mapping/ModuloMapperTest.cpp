// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace cgra::mapping;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json");
}

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

ModuloMapperOptions options(std::uint32_t maxII = 4) {
  ModuloMapperOptions result;
  result.maxII = maxII;
  result.budget.maxNodeCandidateAttempts = 20000;
  result.budget.maxBacktracks = 10000;
  result.budget.maxRouteSearchCalls = 20000;
  result.budget.perRouteBudget.maxStateExpansions = 10000;
  result.budget.perRouteBudget.maxQueuePushes = 20000;
  return result;
}

void expectMapped(const cgra::ir::DFG& generic, const cgra::TargetModel& target, const char* name,
                  std::uint32_t maxII = 4) {
  const auto dfg = legalize(generic, target);
  const auto result = ModuloMapper::map(dfg, target, options(maxII));
  if (!result.ok())
    throw std::runtime_error(std::string(name) + " did not map: " + result.format());
  expect(result.mapping->ii() >= result.stats.startingMII, "mapper never searches below MII");
  expect(ModuloMappingVerifier::verify(dfg, target, *result.mapping).ok(),
         "successful mapper result passes independent T005 verification");
  expect(parse(toJson(*result.mapping)) == *result.mapping, "mapper output round-trips");
}

void testCanonicalGraphs(const cgra::TargetModel& target) {
  expectMapped(cgra::ir::fixtures::simpleAdd(), target, "simple add");
  expectMapped(cgra::ir::fixtures::arithmeticChain(), target, "arithmetic chain");
  expectMapped(cgra::ir::fixtures::fanout(), target, "fanout");
  expectMapped(cgra::ir::fixtures::predicateSelectUnsigned(), target, "predicate select");
  expectMapped(cgra::ir::fixtures::recurrence(), target, "load recurrence");
  expectMapped(cgra::ir::fixtures::loadAddStore(), target, "load add store");
  expectMapped(cgra::ir::fixtures::predicatedStore(), target, "predicated store");
  expectMapped(cgra::ir::fixtures::memoryDependence(), target, "memory dependence");
}

void testCyclicAndDeterministic(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  const auto first = ModuloMapper::map(dfg, target, options(3));
  const auto second = ModuloMapper::map(dfg, target, options(3));
  expect(first.ok() && second.ok(), "cyclic recurrence maps without topological ordering");
  expect(*first.mapping == *second.mapping, "mapper result is deterministic");
  expect(first.stats.nodeCandidateAttempts == second.stats.nodeCandidateAttempts &&
             first.stats.routeSearchCalls == second.stats.routeSearchCalls,
         "mapper statistics are deterministic");
  const auto self = first.mapping->dependence(1);
  expect(self.transport && !self.transport->actions.empty() &&
             std::holds_alternative<VirtualHold>(self.transport->actions.front()),
         "recurrence mapping uses explicit virtual storage for same-tile recurrence");
}

void testBudgetAndMaxII(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  auto tiny = options(4);
  tiny.budget.maxNodeCandidateAttempts = 0;
  const auto budget = ModuloMapper::map(dfg, target, tiny);
  expect(budget.status == ModuloMapperStatus::BudgetExceeded && !budget.mapping,
         "global candidate budget is classified distinctly");

  auto noII = options(0);
  noII.maxII = 0;
  const auto oneII = ModuloMapper::map(dfg, target, noII);
  expect(oneII.ok(), "zero maxII uses the MII attempt as the default bound");
  expect(oneII.stats.iiAttempts == 1, "default maxII performs one MII attempt");

  auto minimumII = options(3);
  minimumII.minII = 2;
  const auto minimum = ModuloMapper::map(dfg, target, minimumII);
  expect(minimum.ok() && minimum.mapping->ii() >= 2,
         "explicit mapper minimum II is respected without searching below it");

  auto routeBudget = options(4);
  routeBudget.budget.perRouteBudget.maxStateExpansions = 0;
  routeBudget.budget.perRouteBudget.maxQueuePushes = 1;
  const auto routeLimited = ModuloMapper::map(dfg, target, routeBudget);
  expect(routeLimited.status == ModuloMapperStatus::RouteBudgetExceeded && !routeLimited.mapping,
         "route budget exhaustion is propagated without II escalation");

  auto belowMII = options(1);
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json targetJson;
  input >> targetJson;
  targetJson["operations"]["ADD"]["issue_occupancy"] = 2;
  const auto path = std::filesystem::temp_directory_path() / "cgra-mapper-mii-target.json";
  {
    std::ofstream output(path);
    output << targetJson.dump(2) << '\n';
  }
  const auto highOccupancyTarget = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  const auto belowDfg = legalize(cgra::ir::fixtures::simpleAdd(), highOccupancyTarget);
  const auto below = ModuloMapper::map(belowDfg, highOccupancyTarget, belowMII);
  if (!(below.status == ModuloMapperStatus::NoMappingWithinIILimit && !below.mapping &&
        below.stats.iiAttempts == 0))
    throw std::runtime_error("maxII below MII is rejected before search: " + below.format());
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testCanonicalGraphs(target);
    testCyclicAndDeterministic(target);
    testBudgetAndMaxII(target);
    std::cout << "modulo mapper tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "modulo mapper tests failed: " << error.what() << '\n';
    return 1;
  }
}
