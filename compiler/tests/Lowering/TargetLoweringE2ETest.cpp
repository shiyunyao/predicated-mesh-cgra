// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"
#include "support/TestArtifacts.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::mapping::ModuloMapperOptions mapperOptions() {
  cgra::mapping::ModuloMapperOptions options;
  options.maxII = 8;
  options.budget.maxNodeCandidateAttempts = 20'000;
  options.budget.maxBacktracks = 10'000;
  options.budget.maxRouteSearchCalls = 20'000;
  options.budget.perRouteBudget.maxStateExpansions = 10'000;
  options.budget.perRouteBudget.maxQueuePushes = 20'000;
  return options;
}

cgra::ir::DFG recurrencePredicatedStore() {
  using namespace cgra::ir;
  DFGBuilder builder("recurrence_predicated_store");
  const auto zero = builder.addConstant(ValueType::i32(), 0);
  const auto one = builder.addConstant(ValueType::i32(), 1);
  const auto limit = builder.addConstant(ValueType::i32(), 2);
  const auto address = builder.addConstant(ValueType::i32(), 17);
  const auto compare = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                       ValueType::predicate(), ICmpPredicate::ULT);
  const auto increment =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto next =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto store =
      builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32(), ValueType::predicate()},
                      ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  const auto boundary = RecurrenceBoundary{{{0, ConstantRef{zero}}}};
  builder.addDataEdge(next, compare, 0, 1, boundary);
  builder.bindConstant(compare, 1, limit);
  builder.addDataEdge(next, increment, 0, 1, boundary);
  builder.bindConstant(increment, 1, one);
  builder.addDataEdge(increment, next, 0);
  builder.bindConstant(next, 1, zero);
  builder.bindConstant(store, 0, address);
  builder.addDataEdge(next, store, 1, 1, boundary);
  builder.addPredicateEdge(compare, store, 2);
  builder.addMemoryEdge(store, store, MemoryDepKind::WAW, 1);
  return builder.finish();
}

std::string shellQuote(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (const char character : path.string()) {
    if (character == '\'')
      quoted += "'\\''";
    else
      quoted += character;
  }
  return quoted + "'";
}

void testNodeIssueUsesResolvedPlacementForRFOperands() {
  const auto targetPath = Root / "target/cgra_v3.json";
  const auto model = cgra::TargetModel::loadFromFile(targetPath);
  cgra::pipeline::CompileDFGOptions options;
  options.tripCount = 4;
  options.targetPath = targetPath;
  options.mapper = mapperOptions();
  const auto result =
      cgra::pipeline::compileGenericDFG(recurrencePredicatedStore(), model, options);
  expect(result.ok(), result.message.c_str());

  const auto artifacts = cgra::test::TestArtifacts::forCase("target_lowering_rf_operand");
  artifacts.writeText("program_manifest.json", result.manifest->json);
  const auto manifest = artifacts.root() / "program_manifest.json";
  const auto checker = Root / "tools/check_schedule.py";
  const auto command = "python3 " + shellQuote(checker) + " " + shellQuote(manifest);
  expect(std::system(command.c_str()) == 0,
         "lowered RF-backed operand must pass the independent schedule checker");
}
} // namespace

int main() {
  try {
    testNodeIssueUsesResolvedPlacementForRFOperands();
    std::cout << "target lowering E2E tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "target lowering E2E tests failed: " << error.what() << '\n';
    return 1;
  }
}
