// SPDX-License-Identifier: MIT
#include "Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cgra::ir {

class DFGTestAccess {
public:
  static DFG missingBoundary() {
    auto dfg = fixtures::recurrence();
    std::get<DataEdgeInfo>(dfg.edges_[1].info).boundary.reset();
    return dfg;
  }

  static DFG malformedBoundary() {
    DFGBuilder builder("malformed_boundary");
    const auto seed = builder.addExternal("seed", ValueType::i32());
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(add, 1, seed);
    builder.addDataEdge(
        add, add, 0, 2,
        RecurrenceBoundary{{{0, ExternalValueRef{seed}}, {0, ExternalValueRef{seed}}}});
    return builder.finish();
  }

  static DFG boundaryTypeMismatch() {
    DFGBuilder builder("boundary_type_mismatch");
    const auto seed = builder.addExternal("seed", ValueType::f32());
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(add, 1, builder.addExternal("value", ValueType::i32()));
    builder.addDataEdge(add, add, 0, 1, RecurrenceBoundary{{{0, ExternalValueRef{seed}}}});
    return builder.finish();
  }

  static DFG duplicateProvider() {
    DFGBuilder builder("duplicate_provider");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto lhs =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto rhs =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(lhs, 0, value);
    builder.bindExternal(lhs, 1, value);
    builder.bindExternal(rhs, 1, value);
    builder.addDataEdge(lhs, rhs, 0);
    auto dfg = builder.finish();
    dfg.edges_.push_back({1, lhs, rhs, 0, DataEdgeInfo{0}});
    return dfg;
  }

  static DFG outOfRangeEdge() {
    DFGBuilder builder("operand_index_out_of_range");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(add, 0, value);
    auto dfg = builder.finish();
    dfg.edges_.push_back({0, add, add, 0, DataEdgeInfo{7}});
    return dfg;
  }

  static DFG unknownReferences() {
    DFGBuilder builder("unknown_references");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(add, 0, value);
    builder.bindExternal(add, 1, value);
    auto dfg = builder.finish();
    dfg.edges_.push_back({0, 77, add, 0, DataEdgeInfo{0}});
    dfg.bindings_.push_back({add, 0, ExternalValueRef{88}});
    return dfg;
  }
};

} // namespace cgra::ir

namespace {

using namespace cgra::ir;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void expectCode(const DFGVerificationReport& report, DFGDiagnosticCode code) {
  if (!report.contains(code))
    throw std::runtime_error("expected diagnostic code is absent: " + std::string(toString(code)) +
                             "\n" + report.format());
}

DFG invalidMissingProvider() {
  DFGBuilder builder("missing_operand_provider");
  builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  return builder.finish();
}

DFG invalidTypeAndMetadata() {
  DFGBuilder builder("type_and_metadata_errors");
  const auto source =
      builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i16()}, ValueType::i32());
  const auto select = builder.addNode(
      Opcode::Select, {ValueType::i32(), ValueType::i32(), ValueType::i16()}, ValueType::i16());
  builder.addDataEdge(source, select, 0);
  builder.addDataEdge(source, select, 1);
  return builder.finish();
}

DFG invalidEdges() {
  DFGBuilder builder("edge_errors");
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::SLT);
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(cmp, 0, value);
  builder.bindExternal(cmp, 1, value);
  builder.bindExternal(add, 1, value);
  builder.bindExternal(load, 0, value);
  builder.addDataEdge(cmp, add, 0);
  builder.addMemoryEdge(add, load, MemoryDepKind::RAW);
  builder.addMemoryEdge(load, load, MemoryDepKind::WAR);
  return builder.finish();
}

DFG genericTargetUnsupported() {
  DFGBuilder builder("generic_target_unsupported");
  const auto lhs = builder.addExternal("lhs", ValueType::i32());
  const auto rhs = builder.addExternal("rhs", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::SLT);
  const auto shift =
      builder.addNode(Opcode::AShr, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(cmp, 0, lhs);
  builder.bindExternal(cmp, 1, rhs);
  builder.bindExternal(shift, 0, lhs);
  builder.bindExternal(shift, 1, rhs);
  return builder.finish();
}

DFG predicateSelectValue() {
  DFGBuilder builder("predicate_select_value");
  const auto lhs = builder.addExternal("lhs", ValueType::i32());
  const auto rhs = builder.addExternal("rhs", ValueType::i32());
  const auto yes = builder.addExternal("yes", ValueType::predicate());
  const auto no = builder.addExternal("no", ValueType::predicate());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::EQ);
  const auto select = builder.addNode(
      Opcode::Select, {ValueType::predicate(), ValueType::predicate(), ValueType::predicate()},
      ValueType::predicate());
  builder.bindExternal(cmp, 0, lhs);
  builder.bindExternal(cmp, 1, rhs);
  builder.addPredicateEdge(cmp, select, 0);
  builder.bindExternal(select, 1, yes);
  builder.bindExternal(select, 2, no);
  return builder.finish();
}

DFG predicateLoad() {
  DFGBuilder builder("predicate_load");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::predicate(),
                                    std::nullopt, MemoryOpInfo{8, false});
  builder.bindExternal(load, 0, address);
  return builder.finish();
}

void testCanonicalFixtures() {
  for (const auto& fixture : fixtures::all()) {
    const auto report = DFGVerifier::verify(fixture);
    expect(report.ok(), "canonical fixture must verify");
  }
}

void testTargetIndependentGenericOps() {
  const auto report = DFGVerifier::verify(genericTargetUnsupported());
  expect(report.ok(), "generic-valid unsupported operations must pass");
  expect(DFGVerifier::verify(predicateSelectValue()).ok(),
         "Select may select a non-void predicate value");
  expect(DFGVerifier::verify(predicateLoad()).ok(), "Load may produce a non-void predicate value");
}

void testDiagnostics() {
  const auto missing = DFGVerifier::verify(invalidMissingProvider());
  expect(!missing.ok(), "missing provider must fail");
  expectCode(missing, DFGDiagnosticCode::DFG_OPERAND_MISSING_PROVIDER);

  const auto types = DFGVerifier::verify(invalidTypeAndMetadata());
  expect(!types.ok(), "invalid types must fail");
  expectCode(types, DFGDiagnosticCode::DFG_ICMP_INVALID_RESULT_TYPE);
  expectCode(types, DFGDiagnosticCode::DFG_ICMP_OPERAND_TYPE_MISMATCH);
  expectCode(types, DFGDiagnosticCode::DFG_SELECT_INVALID_PREDICATE);
  expectCode(types, DFGDiagnosticCode::DFG_SELECT_VALUE_TYPE_MISMATCH);

  const auto edges = DFGVerifier::verify(invalidEdges());
  expect(!edges.ok(), "invalid edges must fail");
  expectCode(edges, DFGDiagnosticCode::DFG_DATA_EDGE_INVALID_SOURCE_TYPE);
  expectCode(edges, DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_SOURCE);
  expectCode(edges, DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DESTINATION);
}

void testStructuralCorpus() {
  expectCode(DFGVerifier::verify(DFGTestAccess::duplicateProvider()),
             DFGDiagnosticCode::DFG_OPERAND_DUPLICATE_PROVIDER);
  expectCode(DFGVerifier::verify(DFGTestAccess::outOfRangeEdge()),
             DFGDiagnosticCode::DFG_OPERAND_INDEX_OUT_OF_RANGE);
  const auto unknown = DFGVerifier::verify(DFGTestAccess::unknownReferences());
  expectCode(unknown, DFGDiagnosticCode::DFG_EDGE_UNKNOWN_SOURCE);
  expectCode(unknown, DFGDiagnosticCode::DFG_BINDING_UNKNOWN_EXTERNAL);
}

void testFocusedOpcodeCorpus() {
  auto verifyFailure = [](DFG graph, DFGDiagnosticCode code) {
    const auto report = DFGVerifier::verify(graph);
    expect(!report.ok(), "focused invalid graph must fail");
    expectCode(report, code);
  };

  {
    DFGBuilder builder("data_type_mismatch");
    const auto value = builder.addExternal("value", ValueType::i16());
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(add, 0, value);
    builder.bindExternal(add, 1, value);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_OPERAND_TYPE_MISMATCH);
  }
  {
    DFGBuilder builder("predicate_type_mismatch");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto select = builder.addNode(
        Opcode::Select, {ValueType::i32(), ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(select, 0, value);
    builder.bindExternal(select, 1, value);
    builder.bindExternal(select, 2, value);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_SELECT_INVALID_PREDICATE);
  }
  {
    DFGBuilder builder("void_value_used");
    const auto address = builder.addExternal("address", ValueType::i32());
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()},
                                       ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
    const auto add =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(store, 0, address);
    builder.bindExternal(store, 1, value);
    builder.addDataEdge(store, add, 0);
    builder.bindExternal(add, 1, value);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_VOID_VALUE_USED);
  }
  {
    DFGBuilder builder("icmp_missing_predicate");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto cmp =
        builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()}, ValueType::predicate());
    builder.bindExternal(cmp, 0, value);
    builder.bindExternal(cmp, 1, value);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_ICMP_MISSING_PREDICATE);
  }
  {
    DFGBuilder builder("load_bad_address");
    const auto address = builder.addExternal("address", ValueType::f32());
    const auto load = builder.addNode(Opcode::Load, {ValueType::f32()}, ValueType::i32(),
                                      std::nullopt, MemoryOpInfo{32, false});
    builder.bindExternal(load, 0, address);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_LOAD_INVALID_ADDRESS_TYPE);
  }
  {
    DFGBuilder builder("store_bad_predicate");
    const auto value = builder.addExternal("value", ValueType::i32());
    const auto store =
        builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32(), ValueType::i32()},
                        ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
    builder.bindExternal(store, 0, value);
    builder.bindExternal(store, 1, value);
    builder.bindExternal(store, 2, value);
    verifyFailure(builder.finish(), DFGDiagnosticCode::DFG_STORE_INVALID_PREDICATE);
  }
}

void testDeterminismAndReadOnly() {
  const auto graph = invalidEdges();
  const auto before = dump(graph);
  const auto first = DFGVerifier::verify(graph);
  const auto second = DFGVerifier::verify(graph);
  expect(first.toJson() == second.toJson(), "diagnostics must be deterministic");
  expect(first.toJson().find("cgra.dfg.verification.v1") != std::string::npos,
         "verification JSON schema");
  expect(dump(graph) == before, "verification must not mutate the graph");
  expect(first.format() == second.format(), "formatted diagnostics must be deterministic");
}

void testRecurrenceBoundaries() {
  expectCode(DFGVerifier::verify(DFGTestAccess::missingBoundary()),
             DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_MISSING);
  expectCode(DFGVerifier::verify(DFGTestAccess::malformedBoundary()),
             DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_DUPLICATE_OFFSET);
  expectCode(DFGVerifier::verify(DFGTestAccess::boundaryTypeMismatch()),
             DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_TYPE_MISMATCH);
}

void testMalformedJsonBoundary() {
  const auto valid = dump(fixtures::simpleAdd());
  static_cast<void>(valid);
  bool rejected = false;
  try {
    static_cast<void>(readJson(std::filesystem::path("/definitely/missing/dfg.json")));
  } catch (const std::exception&) {
    rejected = true;
  }
  expect(rejected, "missing DFG input must be rejected at the input boundary");

  const auto path = std::filesystem::temp_directory_path() / "cgra-dfg-memory-edge.json";
  writeJson(fixtures::memoryDependence(), path);
  std::ifstream stream(path);
  nlohmann::json json;
  stream >> json;
  json["edges"][0]["operand"] = 0;
  std::filesystem::remove(path);
  bool rejectedMemoryOperand = false;
  try {
    static_cast<void>(parse(json.dump()));
  } catch (const std::exception&) {
    rejectedMemoryOperand = true;
  }
  expect(rejectedMemoryOperand, "memory edge operand metadata must be rejected");
}

} // namespace

int main() {
  try {
    testCanonicalFixtures();
    testTargetIndependentGenericOps();
    testDiagnostics();
    testStructuralCorpus();
    testFocusedOpcodeCorpus();
    testDeterminismAndReadOnly();
    testRecurrenceBoundaries();
    testMalformedJsonBoundary();
    std::cout << "CGRA_DFG_VERIFIER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_DFG_VERIFIER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
