// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/Mapping/ExactModuloOracle.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
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

cgra::TargetModel loadTinyTarget(bool singleLsu = false) {
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["array"]["rows"] = 2;
  json["array"]["cols"] = 3;
  json["parameters"]["array_rows"] = 2;
  json["parameters"]["array_cols"] = 3;
  json["lsu"]["enabled_tiles"] =
      singleLsu ? nlohmann::json::array({{{"row", 0}, {"col", 0}, {"port_id", 0}}})
                : nlohmann::json::array({{{"row", 0}, {"col", 0}, {"port_id", 0}},
                                         {{"row", 1}, {"col", 0}, {"port_id", 1}}});
  const auto path =
      std::filesystem::temp_directory_path() /
      (singleLsu ? "cgra-exact-oracle-tiny-single.json" : "cgra-exact-oracle-tiny.json");
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  auto result = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  return result;
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

void testCompletionBacktracking(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  auto retry = options(3);
  std::uint64_t checks = 0;
  retry.completeMappingChecker = [&checks](const cgra::target::TargetDFG&, const cgra::TargetModel&,
                                           const ModuloMapping&) {
    ++checks;
    if (checks == 1)
      return CompleteMappingCheckResult{CompleteMappingDecision::Reject, "rf_infeasible",
                                        "directed first-candidate rejection"};
    return CompleteMappingCheckResult{CompleteMappingDecision::Accept, {}, {}};
  };
  const auto result = ModuloMapper::map(dfg, target, retry);
  expect(result.ok(), "mapper must recover from a rejected complete mapping");
  expect(checks >= 2 && result.stats.completedModuloMappings >= 2,
         "completion rejection must trigger same-II search continuation");
  expect(result.stats.postMappingRejected == 1 && result.stats.rfRejected == 1,
         "completion rejection statistics are not recorded");

  auto abort = options(3);
  abort.completeMappingChecker = [](const cgra::target::TargetDFG&, const cgra::TargetModel&,
                                    const ModuloMapping&) {
    return CompleteMappingCheckResult{CompleteMappingDecision::Abort, "budget_rf",
                                      "directed completion budget"};
  };
  const auto aborted = ModuloMapper::map(dfg, target, abort);
  expect(aborted.status == ModuloMapperStatus::BudgetExceeded && !aborted.mapping,
         "completion budget abort must not become candidate infeasibility");
  expect(aborted.stats.iiAttempts == 1 && aborted.stats.postMappingAbort == 1,
         "completion abort must stop before II escalation");
}

void testBudgetPreservesHigherIIAttempt(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  auto retry = options(2);
  retry.budget.maxNodeCandidateAttempts = 10;
  retry.budget.maxBacktracks = 10;
  retry.completeMappingChecker = [](const cgra::target::TargetDFG&, const cgra::TargetModel&,
                                    const ModuloMapping& mapping) {
    if (mapping.ii() == 1)
      return CompleteMappingCheckResult{CompleteMappingDecision::Reject, "rf_infeasible",
                                        "directed lower-II rejection"};
    return CompleteMappingCheckResult{CompleteMappingDecision::Accept, {}, {}};
  };
  const auto result = ModuloMapper::map(dfg, target, retry);
  expect(result.ok() && result.mapping->ii() == 2,
         "bounded search must preserve a deterministic share for a higher II");
  expect(result.stats.iiAttempts == 2 && result.stats.postMappingRejected > 0,
         "higher-II retry follows real lower-II post-mapping rejection");
  expect(std::ranges::any_of(result.diagnostics,
                             [](const auto& diagnostic) {
                               return diagnostic.code ==
                                      ModuloMapperDiagnosticCode::MAP_II_BUDGET_SHARE_EXHAUSTED;
                             }),
         "successful retry records the lower-II budget share without reporting a global failure");
  expect(std::ranges::none_of(result.diagnostics,
                              [](const auto& diagnostic) {
                                return diagnostic.code ==
                                       ModuloMapperDiagnosticCode::MAP_GLOBAL_BUDGET_EXCEEDED;
                              }),
         "successful retry must not report global budget exhaustion");
}

void testTinyExactOracle(const cgra::TargetModel& target) {
  (void)target;
  const auto tiny = loadTinyTarget();
  const auto dfg = legalize(cgra::ir::fixtures::memoryDependence(), tiny);
  const auto oracle = ExactModuloOracle::solve(dfg, tiny, 1);
  expect(oracle.status == ExactOracleStatus::Feasible && oracle.mapping,
         "independent tiny oracle must find the memory-only fixture");
  expect(ModuloMappingVerifier::verify(dfg, tiny, *oracle.mapping).ok(),
         "oracle witness must pass the independent mapping verifier");
  const auto routed = legalize(cgra::ir::fixtures::loadAddStore(), tiny);
  const auto data = ExactModuloOracle::solve(routed, tiny, 2);
  expect(data.status == ExactOracleStatus::Feasible && data.mapping,
         "tiny oracle independently routes a Data graph");
  expect(ModuloMappingVerifier::verify(routed, tiny, *data.mapping).ok(),
         "Data routed oracle witness passes T005");
  const auto predicate = legalize(cgra::ir::fixtures::predicateSelectUnsigned(), tiny);
  const auto pred = ExactModuloOracle::solve(predicate, tiny, 2);
  expect(pred.status == ExactOracleStatus::Feasible && pred.mapping,
         "tiny oracle independently routes a Predicate graph");
  expect(ModuloMappingVerifier::verify(predicate, tiny, *pred.mapping).ok(),
         "Predicate routed oracle witness passes T005");

  const auto recurrence = legalize(cgra::ir::fixtures::recurrence(), tiny);
  const auto recurrent = ExactModuloOracle::solve(recurrence, tiny, 2);
  expect(recurrent.status == ExactOracleStatus::Feasible && recurrent.mapping,
         "tiny oracle handles a small loop-carried Data graph");
  expect(ModuloMappingVerifier::verify(recurrence, tiny, *recurrent.mapping).ok(),
         "recurrence oracle witness passes T005");

  cgra::ir::DFGBuilder mixedBuilder("oracle_mixed_memory_data");
  const auto address = mixedBuilder.addExternal("address", cgra::ir::ValueType::i32());
  const auto value = mixedBuilder.addExternal("value", cgra::ir::ValueType::i32());
  const auto store = mixedBuilder.addNode(
      cgra::ir::Opcode::Store, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::voidTy(), std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto load = mixedBuilder.addNode(cgra::ir::Opcode::Load, {cgra::ir::ValueType::i32()},
                                         cgra::ir::ValueType::i32(), std::nullopt,
                                         cgra::ir::MemoryOpInfo{32, false});
  const auto add = mixedBuilder.addNode(cgra::ir::Opcode::Add,
                                        {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                        cgra::ir::ValueType::i32());
  mixedBuilder.bindExternal(store, 0, address);
  mixedBuilder.bindExternal(store, 1, value);
  mixedBuilder.bindExternal(load, 0, address);
  mixedBuilder.addMemoryEdge(store, load, cgra::ir::MemoryDepKind::RAW, 0);
  mixedBuilder.addDataEdge(load, add, 0);
  mixedBuilder.bindExternal(add, 1, value);
  const auto mixed = legalize(mixedBuilder.finish(), tiny);
  const auto mixedResult = ExactModuloOracle::solve(mixed, tiny, 2);
  expect(mixedResult.status == ExactOracleStatus::Feasible && mixedResult.mapping,
         "tiny oracle handles mixed Memory and Data edges");
  expect(ModuloMappingVerifier::verify(mixed, tiny, *mixedResult.mapping).ok(),
         "mixed oracle witness passes T005");

  ExactOracleOptions bounds;
  bounds.maxNodes = 1;
  expect(ExactModuloOracle::solve(routed, tiny, 2, bounds).status ==
             ExactOracleStatus::UnsupportedOracleSize,
         "tiny oracle rejects graphs above its node bound");
  bounds = {};
  bounds.maxEdges = 1;
  expect(ExactModuloOracle::solve(routed, tiny, 2, bounds).status ==
             ExactOracleStatus::UnsupportedOracleSize,
         "tiny oracle rejects graphs above its edge bound");
  bounds = {};
  bounds.maxTiles = 2;
  expect(ExactModuloOracle::solve(routed, tiny, 2, bounds).status ==
             ExactOracleStatus::UnsupportedOracleSize,
         "tiny oracle rejects targets above its tile bound");
  bounds = {};
  bounds.maxII = 1;
  expect(ExactModuloOracle::solve(routed, tiny, 2, bounds).status ==
             ExactOracleStatus::UnsupportedOracleSize,
         "tiny oracle rejects II above its bound");

  // A disconnected target plus heterogeneous FU capabilities forces the two
  // endpoints onto different tiles, proving that the oracle reports a real
  // routed infeasibility rather than relying on a same-tile hold.
  std::ifstream disconnectedInput(Root / "target/cgra_v2.json");
  nlohmann::json disconnectedJson;
  disconnectedInput >> disconnectedJson;
  disconnectedJson["array"]["rows"] = 1;
  disconnectedJson["array"]["cols"] = 2;
  disconnectedJson["parameters"]["array_rows"] = 1;
  disconnectedJson["parameters"]["array_cols"] = 2;
  disconnectedJson["interconnect"]["topology"] = "disconnected";
  disconnectedJson["lsu"]["enabled_tiles"] =
      nlohmann::json::array({{{"row", 0}, {"col", 0}, {"port_id", 0}}});
  disconnectedJson["tile_capabilities"]["overrides"] =
      nlohmann::json::array({{{"row", 0}, {"col", 0}, {"operations", {"ADD"}}},
                             {{"row", 0}, {"col", 1}, {"operations", {"SUB"}}}});
  const auto disconnectedPath =
      std::filesystem::temp_directory_path() / "cgra-exact-oracle-disconnected.json";
  {
    std::ofstream output(disconnectedPath);
    output << disconnectedJson.dump(2) << '\n';
  }
  const auto disconnectedTarget = cgra::TargetModel::loadFromFile(disconnectedPath);
  std::filesystem::remove(disconnectedPath);
  cgra::ir::DFGBuilder disconnectedBuilder("oracle_disconnected_route");
  const auto lhs = disconnectedBuilder.addExternal("lhs", cgra::ir::ValueType::i32());
  const auto rhs = disconnectedBuilder.addExternal("rhs", cgra::ir::ValueType::i32());
  const auto third = disconnectedBuilder.addExternal("third", cgra::ir::ValueType::i32());
  const auto disconnectedAdd = disconnectedBuilder.addNode(
      cgra::ir::Opcode::Add, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::i32());
  const auto sub = disconnectedBuilder.addNode(
      cgra::ir::Opcode::Sub, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::i32());
  disconnectedBuilder.bindExternal(disconnectedAdd, 0, lhs);
  disconnectedBuilder.bindExternal(disconnectedAdd, 1, rhs);
  disconnectedBuilder.addDataEdge(disconnectedAdd, sub, 0);
  disconnectedBuilder.bindExternal(sub, 1, third);
  const auto disconnectedDfg = legalize(disconnectedBuilder.finish(), disconnectedTarget);
  const auto noRoute = ExactModuloOracle::solve(disconnectedDfg, disconnectedTarget, 1);
  expect(noRoute.status == ExactOracleStatus::Infeasible,
         "tiny oracle classifies a forced disconnected routed graph as infeasible");

  // Fixed-seed target mutations provide a small deterministic CI-100 corpus:
  // one LSU tile cannot issue the store/load pair in the same modulo slot.
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json targetJson;
  input >> targetJson;
  targetJson["array"]["rows"] = 2;
  targetJson["array"]["cols"] = 3;
  targetJson["parameters"]["array_rows"] = 2;
  targetJson["parameters"]["array_cols"] = 3;
  targetJson["lsu"]["enabled_tiles"] =
      nlohmann::json::array({{{"row", 0}, {"col", 0}, {"port_id", 0}}});
  const auto path = std::filesystem::temp_directory_path() / "cgra-exact-oracle-infeasible.json";
  {
    std::ofstream output(path);
    output << targetJson.dump(2) << '\n';
  }
  const auto constrainedTarget = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  const auto constrainedDfg = legalize(cgra::ir::fixtures::memoryDependence(), constrainedTarget);
  const auto infeasible = ExactModuloOracle::solve(constrainedDfg, constrainedTarget, 1);
  expect(infeasible.status == ExactOracleStatus::Infeasible,
         "independent tiny oracle must classify a constrained memory fixture as infeasible");
}

void testSeededOracleCorpus(const cgra::TargetModel& target) {
  (void)target;
  const auto tiny = loadTinyTarget();
  std::mt19937 generator(0xC1A0100U);
  for (unsigned caseIndex = 0; caseIndex < 8; ++caseIndex) {
    const auto distance = generator() % 2;
    cgra::ir::DFGBuilder builder("oracle_seed_" + std::to_string(caseIndex));
    const auto address = builder.addExternal("address", cgra::ir::ValueType::i32());
    const auto value = builder.addExternal("value", cgra::ir::ValueType::i32());
    const auto store = builder.addNode(
        cgra::ir::Opcode::Store, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
        cgra::ir::ValueType::voidTy(), std::nullopt, cgra::ir::MemoryOpInfo{32, false});
    const auto load = builder.addNode(cgra::ir::Opcode::Load, {cgra::ir::ValueType::i32()},
                                      cgra::ir::ValueType::i32(), std::nullopt,
                                      cgra::ir::MemoryOpInfo{32, false});
    builder.bindExternal(store, 0, address);
    builder.bindExternal(store, 1, value);
    builder.bindExternal(load, 0, address);
    builder.addMemoryEdge(store, load, cgra::ir::MemoryDepKind::RAW, distance);
    const auto dfg = legalize(builder.finish(), tiny);
    const auto first = ExactModuloOracle::solve(dfg, tiny, 1);
    const auto second = ExactModuloOracle::solve(dfg, tiny, 1);
    expect(first.status == second.status, "seeded exact-oracle status is deterministic");
    if (first.status == ExactOracleStatus::Feasible)
      expect(first.mapping && ModuloMappingVerifier::verify(dfg, tiny, *first.mapping).ok(),
             "seeded exact-oracle witness passes T005");
  }

  // Fixed-seed routed comparison corpus. The oracle is deliberately tiny,
  // while the production mapper remains the heuristic under test.
  for (unsigned caseIndex = 0; caseIndex < 12; ++caseIndex) {
    const auto choice = generator() % 4;
    const auto generic = choice == 0   ? cgra::ir::fixtures::arithmeticChain()
                         : choice == 1 ? cgra::ir::fixtures::predicateSelectUnsigned()
                         : choice == 2 ? cgra::ir::fixtures::loadAddStore()
                                       : cgra::ir::fixtures::recurrence();
    const auto dfg = legalize(generic, tiny);
    const auto oracle = ExactModuloOracle::solve(dfg, tiny, 2);
    const auto repeatOracle = ExactModuloOracle::solve(dfg, tiny, 2);
    expect(oracle.status == repeatOracle.status,
           "routed exact-oracle result is deterministic for every seed");
    const auto mapper = ModuloMapper::map(dfg, tiny, options(2));
    if (mapper.ok()) {
      expect(oracle.status != ExactOracleStatus::Infeasible,
             "production Mapper success contradicts routed exact-oracle infeasibility");
      expect(ModuloMappingVerifier::verify(dfg, tiny, *mapper.mapping).ok(),
             "production Mapper success remains independently T005-legal");
    }
    if (oracle.status == ExactOracleStatus::Feasible && !mapper.ok())
      std::cerr << "routed mapper quality miss seed=" << caseIndex
                << " status=" << static_cast<int>(mapper.status) << '\n';
  }
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testCanonicalGraphs(target);
    testCyclicAndDeterministic(target);
    testBudgetAndMaxII(target);
    testCompletionBacktracking(target);
    testBudgetPreservesHigherIIAttempt(target);
    testTinyExactOracle(target);
    testSeededOracleCorpus(target);
    std::cout << "modulo mapper tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "modulo mapper tests failed: " << error.what() << '\n';
    return 1;
  }
}
