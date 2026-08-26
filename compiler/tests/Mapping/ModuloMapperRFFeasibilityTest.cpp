// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef CGRA_REPOSITORY_ROOT
#define CGRA_REPOSITORY_ROOT "."
#endif

namespace {

const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::ir::RecurrenceBoundary boundary(cgra::ir::ConstantId seed) {
  cgra::ir::RecurrenceBoundary result;
  result.values.push_back({0, cgra::ir::ConstantRef{seed}});
  return result;
}

cgra::ir::DFG affineMemoryFanout() {
  using cgra::ir::Opcode;
  using cgra::ir::ValueType;

  cgra::ir::DFGBuilder builder("generic_affine_memory_fanout");
  const auto zero = builder.addConstant(ValueType::i32(), 0);
  const auto one = builder.addConstant(ValueType::i32(), 1);
  const auto baseA = builder.addConstant(ValueType::i32(), 0);
  const auto baseB = builder.addConstant(ValueType::i32(), 64);
  const auto baseC = builder.addConstant(ValueType::i32(), 128);
  const auto add = [&](Opcode opcode = Opcode::Add) {
    return builder.addNode(opcode, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  };

  const auto addressA = add();
  const auto addressB = add();
  const auto addressC = add();
  const auto loadA = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                     std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto loadB = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                     std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto sum = add();
  const auto increment = add();
  const auto latch = add();
  const auto store =
      builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()}, ValueType::voidTy(),
                      std::nullopt, cgra::ir::MemoryOpInfo{32, false});

  builder.bindConstant(addressA, 0, baseA);
  builder.bindConstant(addressB, 0, baseB);
  builder.bindConstant(addressC, 0, baseC);
  builder.addDataEdge(latch, addressA, 1, 1, boundary(zero));
  builder.addDataEdge(latch, addressB, 1, 1, boundary(zero));
  builder.addDataEdge(latch, addressC, 1, 1, boundary(zero));
  builder.addDataEdge(addressA, loadA, 0);
  builder.addDataEdge(addressB, loadB, 0);
  builder.addDataEdge(loadA, sum, 0);
  builder.addDataEdge(loadB, sum, 1);
  builder.addDataEdge(latch, increment, 0, 1, boundary(zero));
  builder.bindConstant(increment, 1, one);
  builder.addDataEdge(increment, latch, 0);
  builder.bindConstant(latch, 1, zero);
  builder.addDataEdge(addressC, store, 0);
  builder.addDataEdge(sum, store, 1);
  return builder.finish();
}

} // namespace

int main() {
  try {
    const auto target = cgra::TargetModel::loadFromFile(Root / "target/cgra_v3.json");
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto artifacts = std::filesystem::temp_directory_path() /
                           ("cgra-generic-rf-feasible-" + std::to_string(stamp));
    cgra::pipeline::CompileDFGOptions options;
    options.tripCount = 4;
    options.targetPath = Root / "target/cgra_v3.json";
    options.artifactDirectory = artifacts;
    options.programName = "generic_affine_memory_fanout";
    options.mapper.minII = 6;
    options.mapper.maxII = 6;
    options.mapper.budget.maxNodeCandidateAttempts = 30'000;
    options.mapper.budget.maxBacktracks = 30'000;
    options.mapper.budget.maxRouteSearchCalls = 60'000;
    options.mapper.budget.perRouteBudget.maxStateExpansions = 10'000;
    options.mapper.budget.perRouteBudget.maxQueuePushes = 20'000;
    options.rfAllocation.budget.maxColoringDecisions = 100'000;
    options.rfAllocation.budget.maxColoringBacktracks = 100'000;

    const auto result = cgra::pipeline::compileGenericDFG(affineMemoryFanout(), target, options);
    std::filesystem::remove_all(artifacts);
    expect(result.ok(),
           "generic recurrence/address fanout must find an RF-feasible mapping: " + result.message);
    expect(result.stats.rfRejected > 0,
           "regression must exercise production RF feasibility backtracking");
    std::cout << "CGRA_GENERIC_RF_FEASIBILITY_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_GENERIC_RF_FEASIBILITY_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
