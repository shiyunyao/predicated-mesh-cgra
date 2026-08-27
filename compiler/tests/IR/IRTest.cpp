// SPDX-License-Identifier: MIT
#include "Fixtures.h"
#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGSerialization.h"
#include "support/TestArtifacts.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace cgra::ir;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void testTypesAndOpcodes() {
  expect(ValueType::i32().toString() == "i32", "i32 string form");
  expect(ValueType::predicate().toString() == "predicate", "predicate string form");
  expect(ValueType::i32() == ValueType::fromString(ValueType::i32().toString()),
         "i32 string round-trip");
  expect(ValueType::predicate() == ValueType::fromString(ValueType::predicate().toString()),
         "predicate string round-trip");
  expect(ValueType::f32() == ValueType::fromString("f32"), "f32 round-trip");
  expect(ValueType::voidTy() == ValueType::fromString("void"), "void round-trip");
  expect(opcodeFromString("AShr") == Opcode::AShr, "generic AShr opcode");
  expect(opcodeFromString("Custom") == Opcode::Custom, "generic Custom opcode");
  expect(icmpPredicateFromString("SLT") == ICmpPredicate::SLT, "signed compare predicate");
}

void testStableIdsAndAdjacency() {
  const auto dfg = fixtures::arithmeticChain();
  expect(dfg.nodes().size() == 3 && dfg.edges().size() == 2, "chain size");
  expect(dfg.node(0).opcode == Opcode::Add && dfg.node(2).opcode == Opcode::Xor,
         "stable node lookup");
  expect(dfg.externalValue(0).name == "a" && dfg.constant(0).bits == 3,
         "stable external/constant lookup");
  expect(dfg.outgoing(0).size() == 1 && dfg.incoming(1).size() == 1, "direct adjacency");
  expect(dfg.edge(dfg.outgoing(0).front()).distance == 0, "edge distance");
}

void testLiveOutAndProviderOwnership() {
  DFGBuilder builder("live_out");
  const auto producer = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                        ValueType::predicate(), ICmpPredicate::EQ);
  const auto consumer =
      builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()},
                      ValueType::i32());
  const auto liveOut = builder.addLiveOut("selected", ValueType::i32(), consumer);
  builder.addPredicateEdge(producer, consumer, 0);
  bool rejected = false;
  try {
    builder.addDataEdge(producer, consumer, 0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "mixed data/predicate providers must be rejected");
  const auto dfg = builder.finish();
  expect(dfg.containsLiveOut(liveOut) && dfg.liveOut(liveOut).source == consumer,
         "live-out lookup");
}

void testRecurrenceAndMemoryEdges() {
  DFGBuilder builder("recurrence");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto initial = builder.addExternal("initial", ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto store =
      builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32(), ValueType::predicate()},
                      ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(load, 0, address);
  builder.addDataEdge(load, add, 0);
  builder.addDataEdge(add, add, 1, 1, RecurrenceBoundary{{{0, ExternalValueRef{initial}}}});
  builder.bindExternal(store, 0, address);
  builder.addDataEdge(add, store, 1);
  builder.addMemoryEdge(store, load, MemoryDepKind::RAW, 1);
  const auto dfg = builder.finish();
  expect(dfg.edges().size() == 4, "recurrence edge count");
  expect(dfg.edge(1).src == dfg.edge(1).dst && dfg.edge(1).distance == 1, "cyclic recurrence edge");
  expect(dfg.edge(3).kind() == Edge::Kind::Memory &&
             std::get<MemoryEdgeInfo>(dfg.edge(3).info).dependence == MemoryDepKind::RAW,
         "memory edge semantics");
  expect(dfg.node(store).resultType == ValueType::voidTy(), "store is void");
}

void testPredicateAndSelect() {
  DFGBuilder builder("predicate_select");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto yes = builder.addExternal("yes", ValueType::i32());
  const auto no = builder.addExternal("no", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::SLT);
  const auto select =
      builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()},
                      ValueType::i32());
  builder.bindExternal(cmp, 0, a);
  builder.bindExternal(cmp, 1, b);
  builder.addPredicateEdge(cmp, select, 0);
  builder.bindExternal(select, 1, yes);
  builder.bindExternal(select, 2, no);
  const auto dfg = builder.finish();
  expect(dfg.edge(0).kind() == Edge::Kind::Predicate, "predicate edge kind");
  expect(dfg.node(0).icmpPredicate == ICmpPredicate::SLT, "icmp metadata");
}

void testSerialization() {
  const auto original = fixtures::arithmeticChain();
  const auto first = cgra::ir::dump(original);
  expect(first == cgra::ir::dump(original), "debug dump deterministic");
  expect(first.find("live-outs:") != std::string::npos, "debug dump includes live-outs");
  const auto artifacts = cgra::test::TestArtifacts::forCase("ir_serialization");
  artifacts.writeText("arithmetic_chain.txt", first);
  const auto path = std::filesystem::temp_directory_path() / "cgra-dfg-ir-test.json";
  cgra::ir::writeJson(original, path);
  artifacts.copyFile("arithmetic_chain.json", path);
  const auto restored = cgra::ir::readJson(path);
  std::filesystem::remove(path);
  expect(original == restored, "JSON semantic round-trip");
  expect(cgra::ir::dump(restored) == first, "round-trip debug dump");

  const auto recurrence = fixtures::recurrence();
  const auto recurrencePath = std::filesystem::temp_directory_path() / "cgra-dfg-recurrence.json";
  cgra::ir::writeJson(recurrence, recurrencePath);
  const auto restoredRecurrence = cgra::ir::readJson(recurrencePath);
  std::filesystem::remove(recurrencePath);
  expect(recurrence == restoredRecurrence, "recurrence boundary JSON semantic round-trip");

  DFGBuilder customBuilder("custom_operation");
  const auto customInput = customBuilder.addExternal("input", ValueType::f32());
  const auto custom =
      customBuilder.addCustomNode("FNEG", {ValueType::f32()}, ValueType::f32());
  customBuilder.bindExternal(custom, 0, customInput);
  const auto customGraph = customBuilder.finish();
  const auto customPath = std::filesystem::temp_directory_path() / "cgra-dfg-custom.json";
  cgra::ir::writeJson(customGraph, customPath);
  const auto restoredCustom = cgra::ir::readJson(customPath);
  std::filesystem::remove(customPath);
  expect(customGraph == restoredCustom && restoredCustom.node(custom).operationKey == "FNEG",
         "Custom operation key survives JSON semantic round-trip");

  const auto sparseSourcePath =
      std::filesystem::temp_directory_path() / "cgra-dfg-sparse-source.json";
  cgra::ir::writeJson(fixtures::simpleAdd(), sparseSourcePath);
  std::ifstream sparseSource(sparseSourcePath);
  nlohmann::json sparse;
  sparseSource >> sparse;
  std::filesystem::remove(sparseSourcePath);
  sparse["external_values"][0]["id"] = 10;
  sparse["external_values"][1]["id"] = 20;
  sparse["nodes"][0]["id"] = 42;
  sparse["live_outs"][0]["id"] = 99;
  sparse["live_outs"][0]["node"] = 42;
  for (auto& binding : sparse["external_bindings"]) {
    binding["node"] = 42;
    if (binding.contains("external"))
      binding["external"] = binding["external"].get<unsigned>() == 0 ? 10 : 20;
  }
  const auto sparseRestored = cgra::ir::parse(sparse.dump());
  expect(sparseRestored.node(42).id == 42 && sparseRestored.externalValue(20).id == 20,
         "sparse semantic IDs survive DFG import");
  const auto sparseOutputPath =
      std::filesystem::temp_directory_path() / "cgra-dfg-sparse-output.json";
  cgra::ir::writeJson(sparseRestored, sparseOutputPath);
  std::ifstream sparseOutput(sparseOutputPath);
  nlohmann::json serializedSparse;
  sparseOutput >> serializedSparse;
  std::filesystem::remove(sparseOutputPath);
  expect(serializedSparse["nodes"][0]["id"] == 42, "sparse IDs remain stable in DFG serialization");
}

void testCanonicalFixtures() {
  const auto fixtures = cgra::ir::fixtures::all();
  expect(fixtures.size() == 8, "canonical fixture count");
  for (const auto& fixture : fixtures) {
    expect(!fixture.name().empty() && !fixture.nodes().empty(), "fixture must be non-empty");
    const auto jsonPath =
        std::filesystem::temp_directory_path() / ("cgra-dfg-" + fixture.name() + ".json");
    cgra::ir::writeJson(fixture, jsonPath);
    expect(fixture == cgra::ir::readJson(jsonPath), "canonical fixture round-trip");
    std::filesystem::remove(jsonPath);
  }
}

} // namespace

int main() {
  try {
    testTypesAndOpcodes();
    testStableIdsAndAdjacency();
    testLiveOutAndProviderOwnership();
    testRecurrenceAndMemoryEdges();
    testPredicateAndSelect();
    testSerialization();
    testCanonicalFixtures();
    std::cout << "CGRA_IR_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_IR_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
