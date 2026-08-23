// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetDFGVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace cgra::target {

class TargetDFGTestAccess {
public:
  static TargetNode& node(TargetDFG& dfg, TargetNodeId id) {
    return dfg.nodes_.at(dfg.nodeIndices_.at(id));
  }

  static TargetEdge& edge(TargetDFG& dfg, TargetEdgeId id) {
    return dfg.edges_.at(dfg.edgeIndices_.at(id));
  }

  static void duplicateBinding(TargetDFG& dfg) { dfg.bindings_.push_back(dfg.bindings_.front()); }

  static void duplicateExternal(TargetDFG& dfg) {
    dfg.externalValues_.push_back(dfg.externalValues_.front());
  }

  static void duplicateConstant(TargetDFG& dfg) {
    dfg.constants_.push_back(dfg.constants_.front());
  }

  static void duplicateLiveOut(TargetDFG& dfg) { dfg.liveOuts_.push_back(dfg.liveOuts_.front()); }

  static void appendUnindexedEdge(TargetDFG& dfg, TargetEdge edge) {
    dfg.edges_.push_back(std::move(edge));
  }
};

} // namespace cgra::target

namespace {

const std::filesystem::path RepositoryRoot = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(RepositoryRoot / "target/cgra_v2.json");
}

cgra::target::TargetDFG legalize(const cgra::ir::DFG& dfg, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(dfg, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

void expectCode(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                cgra::target::TargetDFGDiagnosticCode code) {
  const auto report = cgra::target::TargetDFGVerifier::verify(dfg, target);
  expect(report.contains(code), "expected TargetDFG diagnostic code");
}

void testOperationMetadata(const cgra::TargetModel& target) {
  auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).operation = "UNKNOWN";
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_UNKNOWN_OPERATION);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).executionClass = cgra::TargetExecutionClass::LSU;
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_EXECUTION_CLASS_MISMATCH);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).issueOccupancy = 2;
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_ISSUE_OCCUPANCY_MISMATCH);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).resultLatency = 4;
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_RESULT_LATENCY_MISMATCH);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).resultType = cgra::ir::ValueType::i16();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).operandTypes.pop_back();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_OPERATION_ARITY_INVALID);
}

void testProvidersAndEdges(const cgra::TargetModel& target) {
  auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::duplicateBinding(dfg);
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_DUPLICATE_PROVIDER);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::duplicateExternal(dfg);
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_DUPLICATE_EXTERNAL);

  dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  cgra::target::TargetDFGTestAccess::duplicateConstant(dfg);
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_DUPLICATE_CONSTANT);

  dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  cgra::target::TargetDFGTestAccess::duplicateLiveOut(dfg);
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_DUPLICATE_LIVEOUT);

  dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  cgra::target::TargetDFGTestAccess::edge(dfg, 0).src = 999;
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_UNKNOWN_EDGE_SOURCE);

  dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  cgra::target::TargetDFGTestAccess::appendUnindexedEdge(dfg,
                                                         {999, 0, 1, 0, cgra::ir::DataEdgeInfo{0}});
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_ADJACENCY_INCONSISTENT);

  dfg = legalize(cgra::ir::fixtures::predicateSelectUnsigned(), target);
  cgra::target::TargetDFGTestAccess::edge(dfg, 0).info = cgra::ir::DataEdgeInfo{0};
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_DATA_EDGE_INVALID);

  dfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  cgra::target::TargetDFGTestAccess::edge(dfg, 0).info =
      cgra::ir::MemoryEdgeInfo{cgra::ir::MemoryDepKind::WAR};
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_MEMORY_EDGE_INVALID);
}

void testMemoryNodeRules(const cgra::TargetModel& target) {
  auto dfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 2).resultType = cgra::ir::ValueType::i32();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_STORE_RESULT_INVALID);

  dfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 2).operandTypes[0] =
      cgra::ir::ValueType::predicate();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_OPERATION_OPERAND_INVALID);

  dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).resultLatency.reset();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_LOAD_RESULT_INVALID);

  dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  cgra::target::TargetDFGTestAccess::node(dfg, 0).operandTypes[0] =
      cgra::ir::ValueType::predicate();
  expectCode(dfg, target, cgra::target::TargetDFGDiagnosticCode::TDFG_OPERATION_OPERAND_INVALID);
}

void testIndependentSparseIds() {
  cgra::target::TargetDFGBuilder builder("sparse", "cgra_v2_shared4p");
  builder.addNode(
      {10, "NOP", cgra::TargetExecutionClass::FU, cgra::ir::ValueType::i32(), {}, 1, 1, {7}});
  builder.addNode(
      {42, "NOP", cgra::TargetExecutionClass::FU, cgra::ir::ValueType::i32(), {}, 1, 1, {8}});
  const auto dfg = builder.finish();
  expect(cgra::target::parse(cgra::target::toJson(dfg)) == dfg,
         "Target DFG preserves independent sparse IDs");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testOperationMetadata(target);
    testProvidersAndEdges(target);
    testMemoryNodeRules(target);
    testIndependentSparseIds();
    std::cout << "CGRA_TARGET_DFG_VERIFIER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_TARGET_DFG_VERIFIER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
