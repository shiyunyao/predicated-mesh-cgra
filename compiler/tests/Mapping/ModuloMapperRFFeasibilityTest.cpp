// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/ABI/CompileKernel.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"
#include "support/TestArtifacts.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ranges>
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

cgra::ir::DFG fixedRegisterOverlapCandidate() {
  using cgra::ir::Opcode;
  using cgra::ir::ValueType;

  cgra::ir::DFGBuilder builder("generic_fixed_register_overlap_candidate");
  const auto zero = builder.addConstant(ValueType::i32(), 0);
  const auto one = builder.addConstant(ValueType::i32(), 1);
  const auto baseA = builder.addConstant(ValueType::i32(), 0);
  const auto baseB = builder.addConstant(ValueType::i32(), 64);
  const auto baseC = builder.addConstant(ValueType::i32(), 128);
  const auto add = [&] {
    return builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  };

  const auto addressA = add();
  const auto loadA = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                     std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto addressB = add();
  const auto loadB = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                     std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto sum = add();
  const auto addressC = add();
  const auto increment = add();
  const auto store =
      builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()}, ValueType::voidTy(),
                      std::nullopt, cgra::ir::MemoryOpInfo{32, false});

  builder.bindConstant(addressA, 0, baseA);
  builder.addDataEdge(increment, addressA, 1, 1, boundary(zero));
  builder.addDataEdge(addressA, loadA, 0);
  builder.bindConstant(addressB, 0, baseB);
  builder.addDataEdge(increment, addressB, 1, 1, boundary(zero));
  builder.addDataEdge(addressB, loadB, 0);
  builder.addDataEdge(loadA, sum, 0);
  builder.addDataEdge(loadB, sum, 1);
  builder.bindConstant(addressC, 0, baseC);
  builder.addDataEdge(increment, addressC, 1, 1, boundary(zero));
  builder.addDataEdge(addressC, store, 0);
  builder.addDataEdge(sum, store, 1);
  builder.addDataEdge(increment, increment, 0, 1, boundary(zero));
  builder.bindConstant(increment, 1, one);
  return builder.finish();
}

cgra::ir::DFG typedFloatCandidate() {
  cgra::ir::DFGBuilder builder("typed_float_mapping_candidate");
  const auto lhs = builder.addExternal("lhs", cgra::ir::ValueType::floating(64));
  const auto rhs = builder.addExternal("rhs", cgra::ir::ValueType::floating(64));
  const auto add = builder.addCustomNode(
      "FADD", {cgra::ir::ValueType::floating(64), cgra::ir::ValueType::floating(64)},
      cgra::ir::ValueType::floating(64));
  builder.bindExternal(add, 0, lhs);
  builder.bindExternal(add, 1, rhs);
  builder.addLiveOut("sum", cgra::ir::ValueType::floating(64), add);
  return builder.finish();
}

} // namespace

int main() {
  try {
    const auto target = cgra::TargetModel::loadFromFile(Root / "target/cgra_v3.json");
    const auto artifacts =
        cgra::test::TestArtifacts::forCase("modulo_mapper_rf_feasible_affine_fanout");
    cgra::pipeline::CompileDFGOptions options;
    options.tripCount = 4;
    options.targetPath = Root / "target/cgra_v3.json";
    options.artifactDirectory = artifacts.root();
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
    expect(result.ok(),
           "generic recurrence/address fanout must find an RF-feasible mapping: " + result.message);

    options.artifactDirectory =
        cgra::test::TestArtifacts::forCase("mapping_research_fixed_rf_overlap").root();
    options.mode = cgra::pipeline::CompileDFGMode::MappingResearch;
    options.mapper.minII = 1;
    options.mapper.maxII = 2;
    options.mapper.budget.maxNodeCandidateAttempts = 500;
    options.mapper.budget.maxBacktracks = 500;
    options.mapper.budget.maxRouteSearchCalls = 1000;
    options.mapper.budget.perRouteBudget.maxStateExpansions = 1000;
    options.mapper.budget.perRouteBudget.maxQueuePushes = 2000;
    const auto research =
        cgra::pipeline::compileGenericDFG(fixedRegisterOverlapCandidate(), target, options);
    expect(!research.ok() &&
               research.status == cgra::pipeline::CompileDFGStatus::RFConstrainedMappingFailure,
           "RF-constrained research must reject a mapping with no legal finite-RF realization");
    expect(!research.moduloMapping.has_value() && research.stats.completedModuloMappings > 0 &&
               research.stats.rfRejected > 0,
           "raw candidates and RF rejections must remain observable separately");
    expect(research.stats.rfRejectedByII.contains(2) &&
               research.stats.rfRejectedByReason.at("rf_fixed_register_self_overlap") > 0,
           "RF-constrained rejection telemetry must retain II and reason breakdowns");
    expect(!research.manifest.has_value(), "mapping research must not emit a hardware manifest");
    expect(research.stats.mapperInvoked,
           "mapping research must record that the modulo mapper was invoked");

    const auto research64 =
        cgra::TargetModel::loadFromFile(Root / "target/cgra_mapping64_v1.json");
    cgra::abi::CompileKernelOptions typedOptions;
    typedOptions.invocation = {4, {{0, 0x3ff0000000000000ULL},
                                   {1, 0x4000000000000000ULL}},
                               {}};
    typedOptions.backend.mode = cgra::pipeline::CompileDFGMode::MappingResearch;
    typedOptions.backend.mapper.maxII = 4;
    typedOptions.backend.mapper.budget.maxNodeCandidateAttempts = 10'000;
    typedOptions.backend.mapper.budget.maxBacktracks = 10'000;
    typedOptions.backend.mapper.budget.maxRouteSearchCalls = 10'000;
    const auto typed =
        cgra::abi::compileKernel(typedFloatCandidate(), research64, typedOptions);
    expect(typed.ok() && typed.backend->moduloMapping.has_value(),
           "research64 typed kernel must reach verified modulo mapping: " + typed.message);
    expect(typed.backend->stats.mapperInvoked,
           "typed research pipeline must expose mapper invocation telemetry");
    expect(typed.abiLayout && typed.abiLayout->outputs.empty(),
           "mapping specialization must not materialize hardware ABI output Stores");
    expect(typed.bound && typed.bound->dfg.liveOuts().size() == 1 &&
               std::ranges::none_of(typed.bound->dfg.nodes(), [](const auto& node) {
                 return node.opcode == cgra::ir::Opcode::Store;
               }),
           "mapping specialization must preserve LiveOut semantics without physical Stores");

    cgra::pipeline::CompileDFGOptions invalidHardwareOptions;
    invalidHardwareOptions.mode = cgra::pipeline::CompileDFGMode::HardwareExecutable;
    invalidHardwareOptions.mapper.maxII = 4;
    const auto invalidHardware = cgra::pipeline::compileGenericDFG(
        typedFloatCandidate(), research64, invalidHardwareOptions);
    expect(!invalidHardware.ok() &&
               invalidHardware.status ==
                   cgra::pipeline::CompileDFGStatus::TargetLegalizationFailure &&
               invalidHardware.message.find("TARGET_MAPPING_PROFILE_NOT_HARDWARE_EXECUTABLE") !=
                   std::string::npos &&
               !invalidHardware.manifest,
           "abstract mapping target must never produce a hardware manifest");
    std::cout << "CGRA_GENERIC_RF_FEASIBILITY_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_GENERIC_RF_FEASIBILITY_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
