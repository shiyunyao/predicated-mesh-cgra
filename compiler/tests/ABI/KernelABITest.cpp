// SPDX-License-Identifier: MIT
#include "cgra/ABI/CompileKernel.h"
#include "cgra/ABI/KernelABIBinder.h"
#include "cgra/ABI/KernelABIVerifier.h"
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>

namespace {
std::filesystem::path targetPath() {
#ifdef CGRA_REPOSITORY_ROOT
  return std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v3.json";
#else
  return std::filesystem::path("../target/cgra_v3.json");
#endif
}
} // namespace

TEST(KernelABI, BindsInputsAndLiveOutsWithoutChangingSource) {
  using namespace cgra::ir;
  DFGBuilder builder("abi_scalar_add");
  const auto x = builder.addExternal("x", ValueType::i32());
  const auto y = builder.addExternal("y", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(add, 0, x);
  builder.bindExternal(add, 1, y);
  builder.addLiveOut("result", ValueType::i32(), add);
  const auto source = builder.finish();
  const auto original = source;

  const auto target = cgra::TargetModel::loadFromFile(targetPath());
  cgra::abi::KernelInvocation invocation{4, {{x, 7}, {y, 11}}, {}};
  const auto result = cgra::abi::KernelABIBinder::bind(source, target, invocation);
  ASSERT_TRUE(result.ok()) << result.format();
  ASSERT_TRUE(result.bound.has_value());
  EXPECT_EQ(source, original);
  EXPECT_EQ(result.bound->layout.outputs.size(), 1U);
  EXPECT_EQ(result.bound->layout.outputRegionBase, target.memory().depth - 1);
  EXPECT_EQ(result.bound->layout.outputs.front().scratchpadAddress, target.memory().depth - 1);
  EXPECT_EQ(result.bound->dfg.externalBindings().size(), 3U);
  for (const auto& binding : result.bound->dfg.externalBindings())
    EXPECT_TRUE(std::holds_alternative<ConstantRef>(binding.source));
  const auto verification =
      cgra::abi::KernelABIVerifier::verify(source, target, invocation, *result.bound);
  EXPECT_TRUE(verification.ok()) << verification.format();
}

TEST(KernelABI, RejectsMissingInputAndReservedScratchpadCollision) {
  using namespace cgra::ir;
  DFGBuilder builder("abi_invalid");
  const auto x = builder.addExternal("x", ValueType::i32());
  const auto node =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(node, 0, x);
  const auto y = builder.addConstant(ValueType::i32(), 1);
  builder.bindConstant(node, 1, y);
  builder.addLiveOut("result", ValueType::i32(), node);
  const auto source = builder.finish();
  const auto target = cgra::TargetModel::loadFromFile(targetPath());

  cgra::abi::KernelInvocation missing{1, {}, {}};
  EXPECT_FALSE(cgra::abi::KernelABIBinder::bind(source, target, missing).ok());

  cgra::abi::KernelInvocation collision{1, {{x, 3}}, {{target.memory().depth - 1, 9}}};
  const auto result = cgra::abi::KernelABIBinder::bind(source, target, collision);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, cgra::abi::KernelABIBindingStatus::ScratchpadABIRegionConflict);

  DFGBuilder addressBuilder("abi_static_address_collision");
  const auto outputAddress =
      addressBuilder.importConstant({0, ValueType::i32(), target.memory().depth - 1});
  const auto load = addressBuilder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                           std::nullopt, MemoryOpInfo{32, false});
  addressBuilder.bindConstant(load, 0, outputAddress);
  addressBuilder.addLiveOut("value", ValueType::i32(), load);
  const auto staticCollision =
      cgra::abi::KernelABIBinder::bind(addressBuilder.finish(), target, {1, {}, {}});
  EXPECT_EQ(staticCollision.status, cgra::abi::KernelABIBindingStatus::ScratchpadABIRegionConflict);
}

TEST(KernelABI, SpecializesRecurrenceBoundaryWithoutChangingDistance) {
  using namespace cgra::ir;
  DFGBuilder builder("abi_recurrence");
  const auto seed = builder.addExternal("seed", ValueType::i32());
  const auto one = builder.addConstant(ValueType::i32(), 1);
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindConstant(add, 1, one);
  RecurrenceBoundary boundary;
  boundary.values.push_back({0, ExternalValueRef{seed}});
  const auto edge = builder.addDataEdge(add, add, 0, 1, boundary);
  builder.addLiveOut("result", ValueType::i32(), add);
  const auto source = builder.finish();
  const auto target = cgra::TargetModel::loadFromFile(targetPath());
  const auto result = cgra::abi::KernelABIBinder::bind(source, target, {4, {{seed, 5}}, {}});
  ASSERT_TRUE(result.ok()) << result.format();
  const auto& boundEdge = result.bound->dfg.edge(edge);
  const auto& boundBoundary = std::get<DataEdgeInfo>(boundEdge.info).boundary;
  ASSERT_TRUE(boundBoundary.has_value());
  ASSERT_EQ(boundBoundary->values.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<ConstantRef>(boundBoundary->values.front().value));
  EXPECT_EQ(boundEdge.distance, 1U);
  EXPECT_EQ(result.bound->dfg.edge(edge).src, add);
}

TEST(KernelABI, CompileKernelProducesValidatedSemanticManifest) {
  using namespace cgra::ir;
  DFGBuilder builder("abi_compile_smoke");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(add, 0, a);
  builder.bindExternal(add, 1, b);
  builder.addLiveOut("result", ValueType::i32(), add);
  const auto source = builder.finish();
  const auto target = cgra::TargetModel::loadFromFile(targetPath());
  cgra::abi::CompileKernelOptions options;
  options.invocation = {2, {{a, 7}, {b, 7}}, {}};
  options.backend.targetPath = targetPath();
  options.backend.mapper.maxII = 8;
  options.backend.mapper.budget.maxNodeCandidateAttempts = 100000;
  options.backend.mapper.budget.maxBacktracks = 50000;
  options.backend.mapper.budget.maxRouteSearchCalls = 100000;
  options.backend.mapper.budget.perRouteBudget.maxStateExpansions = 10000;
  options.backend.mapper.budget.perRouteBudget.maxQueuePushes = 20000;
  options.backend.rfAllocation.budget.maxColoringDecisions = 100000;
  options.backend.rfAllocation.budget.maxColoringBacktracks = 100000;
  options.backend.materializationBudget.maxExplicitBoundaryCycles = 1000000;
  options.backend.materializationBudget.maxExplicitBoundaryEvents = 1000000;
  const auto result = cgra::abi::compileKernel(source, target, options);
  ASSERT_TRUE(result.ok()) << result.message;
  ASSERT_TRUE(result.backend->manifest.has_value());
  const auto manifest = nlohmann::json::parse(result.backend->manifest->json);
  EXPECT_EQ(manifest.at("schema"), "cgra.program_manifest.v1");
  EXPECT_EQ(result.abiLayout->outputs.size(), 1U);
  EXPECT_EQ(result.abiLayout->outputs.front().scratchpadAddress, target.memory().depth - 1);
}
