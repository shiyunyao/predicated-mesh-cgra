// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetDFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cgra::target {
namespace {

using Json = nlohmann::json;

bool isPredicate(const ir::ValueType& type) {
  return type.kind == ir::ValueKind::Predicate && type.bitWidth == 1;
}

bool isData(const ir::ValueType& type) {
  return type.kind == ir::ValueKind::Integer || type.kind == ir::ValueKind::Float;
}

bool roleMatches(TargetOperandRole role, const ir::ValueType& type) {
  switch (role) {
  case TargetOperandRole::Data:
    return isData(type);
  case TargetOperandRole::Predicate:
    return isPredicate(type);
  case TargetOperandRole::Address:
    return type.kind == ir::ValueKind::Integer;
  }
  return false;
}

bool resultRoleMatches(TargetResultRole role, const ir::ValueType& type) {
  switch (role) {
  case TargetResultRole::Data:
    return isData(type);
  case TargetResultRole::Predicate:
    return isPredicate(type);
  case TargetResultRole::Void:
    return type == ir::ValueType::voidTy();
  }
  return false;
}

bool isLoadLike(const TargetOperationDesc& operation) {
  return operation.executionClass == TargetExecutionClass::LSU &&
         operation.resultRole == TargetResultRole::Data && operation.operands.size() == 1 &&
         operation.operands.front().role == TargetOperandRole::Address;
}

bool isStoreLike(const TargetOperationDesc& operation) {
  return operation.executionClass == TargetExecutionClass::LSU &&
         operation.resultRole == TargetResultRole::Void && operation.operands.size() >= 2 &&
         operation.operands[0].role == TargetOperandRole::Address &&
         operation.operands[1].role == TargetOperandRole::Data;
}

} // namespace

std::string_view toString(TargetDFGDiagnosticCode code) {
  switch (code) {
  case TargetDFGDiagnosticCode::TDFG_UNKNOWN_NODE:
    return "TDFG_UNKNOWN_NODE";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_NODE:
    return "TDFG_DUPLICATE_NODE";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_EDGE:
    return "TDFG_DUPLICATE_EDGE";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_EXTERNAL:
    return "TDFG_DUPLICATE_EXTERNAL";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_CONSTANT:
    return "TDFG_DUPLICATE_CONSTANT";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_LIVEOUT:
    return "TDFG_DUPLICATE_LIVEOUT";
  case TargetDFGDiagnosticCode::TDFG_ADJACENCY_INCONSISTENT:
    return "TDFG_ADJACENCY_INCONSISTENT";
  case TargetDFGDiagnosticCode::TDFG_UNKNOWN_EDGE_SOURCE:
    return "TDFG_UNKNOWN_EDGE_SOURCE";
  case TargetDFGDiagnosticCode::TDFG_UNKNOWN_EDGE_DESTINATION:
    return "TDFG_UNKNOWN_EDGE_DESTINATION";
  case TargetDFGDiagnosticCode::TDFG_UNKNOWN_OPERATION:
    return "TDFG_UNKNOWN_OPERATION";
  case TargetDFGDiagnosticCode::TDFG_NO_COMPATIBLE_EXECUTION_RESOURCE:
    return "TDFG_NO_COMPATIBLE_EXECUTION_RESOURCE";
  case TargetDFGDiagnosticCode::TDFG_EXECUTION_CLASS_MISMATCH:
    return "TDFG_EXECUTION_CLASS_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_ISSUE_OCCUPANCY_MISMATCH:
    return "TDFG_ISSUE_OCCUPANCY_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_RESULT_LATENCY_MISMATCH:
    return "TDFG_RESULT_LATENCY_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_PRODUCER_OUTPUT_READY_MISMATCH:
    return "TDFG_PRODUCER_OUTPUT_READY_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_MEMORY_ACCESS_WIDTH_MISMATCH:
    return "TDFG_MEMORY_ACCESS_WIDTH_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_RESULT_TYPE_MISMATCH:
    return "TDFG_RESULT_TYPE_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE:
    return "TDFG_UNSUPPORTED_TYPE";
  case TargetDFGDiagnosticCode::TDFG_OPERAND_INDEX_OUT_OF_RANGE:
    return "TDFG_OPERAND_INDEX_OUT_OF_RANGE";
  case TargetDFGDiagnosticCode::TDFG_MISSING_PROVIDER:
    return "TDFG_MISSING_PROVIDER";
  case TargetDFGDiagnosticCode::TDFG_DUPLICATE_PROVIDER:
    return "TDFG_DUPLICATE_PROVIDER";
  case TargetDFGDiagnosticCode::TDFG_DATA_EDGE_INVALID:
    return "TDFG_DATA_EDGE_INVALID";
  case TargetDFGDiagnosticCode::TDFG_PREDICATE_EDGE_INVALID:
    return "TDFG_PREDICATE_EDGE_INVALID";
  case TargetDFGDiagnosticCode::TDFG_MEMORY_EDGE_INVALID:
    return "TDFG_MEMORY_EDGE_INVALID";
  case TargetDFGDiagnosticCode::TDFG_STORE_RESULT_INVALID:
    return "TDFG_STORE_RESULT_INVALID";
  case TargetDFGDiagnosticCode::TDFG_LOAD_RESULT_INVALID:
    return "TDFG_LOAD_RESULT_INVALID";
  case TargetDFGDiagnosticCode::TDFG_OPERATION_ARITY_INVALID:
    return "TDFG_OPERATION_ARITY_INVALID";
  case TargetDFGDiagnosticCode::TDFG_OPERATION_OPERAND_INVALID:
    return "TDFG_OPERATION_OPERAND_INVALID";
  case TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_NODE:
    return "TDFG_BINDING_UNKNOWN_NODE";
  case TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_EXTERNAL:
    return "TDFG_BINDING_UNKNOWN_EXTERNAL";
  case TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_CONSTANT:
    return "TDFG_BINDING_UNKNOWN_CONSTANT";
  case TargetDFGDiagnosticCode::TDFG_BINDING_TYPE_MISMATCH:
    return "TDFG_BINDING_TYPE_MISMATCH";
  case TargetDFGDiagnosticCode::TDFG_PROVENANCE_EMPTY:
    return "TDFG_PROVENANCE_EMPTY";
  case TargetDFGDiagnosticCode::TDFG_PROVENANCE_UNKNOWN_GENERIC_NODE:
    return "TDFG_PROVENANCE_UNKNOWN_GENERIC_NODE";
  case TargetDFGDiagnosticCode::TDFG_LIVEOUT_UNKNOWN_NODE:
    return "TDFG_LIVEOUT_UNKNOWN_NODE";
  case TargetDFGDiagnosticCode::TDFG_LIVEOUT_TYPE_MISMATCH:
    return "TDFG_LIVEOUT_TYPE_MISMATCH";
  }
  return "TDFG_UNKNOWN_DIAGNOSTIC";
}

bool TargetDFGVerificationReport::contains(TargetDFGDiagnosticCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string TargetDFGVerificationReport::format() const {
  std::ostringstream output;
  output << (ok() ? "valid" : "invalid") << " Target DFG";
  for (const auto& diagnostic : diagnostics_) {
    output << '\n' << toString(diagnostic.code);
    if (diagnostic.node)
      output << " node=%n" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=%e" << *diagnostic.edge;
    if (diagnostic.operand)
      output << " operand=" << *diagnostic.operand;
    output << ": " << diagnostic.message;
  }
  return output.str();
}

std::string TargetDFGVerificationReport::toJson() const {
  Json root = {{"schema", "cgra.target_dfg.verification.v1"},
               {"valid", ok()},
               {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics_) {
    Json value = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.node)
      value["node"] = *diagnostic.node;
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    if (diagnostic.operand)
      value["operand"] = *diagnostic.operand;
    root["diagnostics"].push_back(std::move(value));
  }
  return root.dump(2) + '\n';
}

TargetDFGVerificationReport
TargetDFGVerifier::verify(const TargetDFG& dfg, const TargetModel& target, const ir::DFG* generic) {
  TargetDFGVerificationReport report;
  std::unordered_map<TargetNodeId, const TargetNode*> nodes;
  std::unordered_map<TargetEdgeId, const TargetEdge*> edges;
  for (const auto& node : dfg.nodes())
    if (!nodes.emplace(node.id, &node).second)
      report.add(
          {TargetDFGDiagnosticCode::TDFG_DUPLICATE_NODE, "duplicate target node ID", node.id});
  for (const auto& edge : dfg.edges())
    if (!edges.emplace(edge.id, &edge).second)
      report.add({TargetDFGDiagnosticCode::TDFG_DUPLICATE_EDGE, "duplicate target edge ID",
                  std::nullopt, edge.id});

  std::unordered_set<ir::ExternalValueId> externalIds;
  for (const auto& value : dfg.externalValues())
    if (!externalIds.insert(value.id).second)
      report.add(
          {TargetDFGDiagnosticCode::TDFG_DUPLICATE_EXTERNAL, "duplicate target external value ID"});
  std::unordered_set<ir::ConstantId> constantIds;
  for (const auto& value : dfg.constants())
    if (!constantIds.insert(value.id).second)
      report.add(
          {TargetDFGDiagnosticCode::TDFG_DUPLICATE_CONSTANT, "duplicate target constant ID"});
  std::unordered_set<ir::LiveOutId> liveOutIds;
  for (const auto& value : dfg.liveOuts())
    if (!liveOutIds.insert(value.id).second)
      report.add({TargetDFGDiagnosticCode::TDFG_DUPLICATE_LIVEOUT, "duplicate target live-out ID"});

  std::map<std::pair<TargetNodeId, std::uint32_t>, unsigned> providers;
  for (const auto& binding : dfg.operandBindings()) {
    const auto nodeIt = nodes.find(binding.node);
    if (nodeIt == nodes.end()) {
      report.add({TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_NODE,
                  "binding references an unknown target node", binding.node});
      continue;
    }
    if (binding.operand >= nodeIt->second->operandTypes.size()) {
      report.add({TargetDFGDiagnosticCode::TDFG_OPERAND_INDEX_OUT_OF_RANGE,
                  "binding operand is outside the target operand list", binding.node, std::nullopt,
                  binding.operand});
      continue;
    }
    ++providers[{binding.node, binding.operand}];
    const auto& expected = nodeIt->second->operandTypes[binding.operand];
    if (std::holds_alternative<ir::ExternalValueRef>(binding.source)) {
      const auto id = std::get<ir::ExternalValueRef>(binding.source).value;
      const auto it = std::find_if(dfg.externalValues().begin(), dfg.externalValues().end(),
                                   [id](const auto& value) { return value.id == id; });
      if (it == dfg.externalValues().end())
        report.add({TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_EXTERNAL,
                    "binding references an unknown external value", binding.node});
      else if (it->type != expected)
        report.add({TargetDFGDiagnosticCode::TDFG_BINDING_TYPE_MISMATCH,
                    "external binding type does not match operand type", binding.node, std::nullopt,
                    binding.operand});
    } else {
      const auto id = std::get<ir::ConstantRef>(binding.source).value;
      const auto it = std::find_if(dfg.constants().begin(), dfg.constants().end(),
                                   [id](const auto& value) { return value.id == id; });
      if (it == dfg.constants().end())
        report.add({TargetDFGDiagnosticCode::TDFG_BINDING_UNKNOWN_CONSTANT,
                    "binding references an unknown constant", binding.node});
      else if (it->type != expected)
        report.add({TargetDFGDiagnosticCode::TDFG_BINDING_TYPE_MISMATCH,
                    "constant binding type does not match operand type", binding.node, std::nullopt,
                    binding.operand});
    }
  }

  for (const auto& edge : dfg.edges()) {
    if (edge.kind() == ir::Edge::Kind::Memory)
      continue;
    const auto operand = edge.kind() == ir::Edge::Kind::Data
                             ? std::get<ir::DataEdgeInfo>(edge.info).dstOperand
                             : std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
    ++providers[{edge.dst, operand}];
  }

  for (const auto& value : dfg.externalValues()) {
    if (!target.supportsValueType(value.type))
      report.add({TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE,
                  "external value type is not supported by TargetModel"});
  }
  for (const auto& value : dfg.constants()) {
    if (!target.supportsValueType(value.type))
      report.add({TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE,
                  "constant type is not supported by TargetModel"});
  }

  for (const auto& node : dfg.nodes()) {
    const auto* operation = target.findOperation(node.operation);
    if (!operation) {
      report.add({TargetDFGDiagnosticCode::TDFG_UNKNOWN_OPERATION,
                  "target operation is absent from TargetModel", node.id});
      continue;
    }
    if (!target.isOperationExecutable(*operation))
      report.add({TargetDFGDiagnosticCode::TDFG_NO_COMPATIBLE_EXECUTION_RESOURCE,
                  "target operation has no compatible execution resource", node.id});
    if (operation->executionClass != node.executionClass)
      report.add({TargetDFGDiagnosticCode::TDFG_EXECUTION_CLASS_MISMATCH,
                  "node execution class disagrees with TargetModel", node.id});
    if (operation->issueOccupancy != node.issueOccupancy)
      report.add({TargetDFGDiagnosticCode::TDFG_ISSUE_OCCUPANCY_MISMATCH,
                  "node issue occupancy disagrees with TargetModel", node.id});
    if (operation->resultLatency != node.resultLatency)
      report.add({TargetDFGDiagnosticCode::TDFG_RESULT_LATENCY_MISMATCH,
                  "node result latency disagrees with TargetModel", node.id});
    if (operation->producerOutputReadyOffset != node.producerOutputReadyOffset)
      report.add({TargetDFGDiagnosticCode::TDFG_PRODUCER_OUTPUT_READY_MISMATCH,
                  "node producer output readiness disagrees with TargetModel", node.id});
    if (operation->accessWidthBits != node.accessWidthBits)
      report.add({TargetDFGDiagnosticCode::TDFG_MEMORY_ACCESS_WIDTH_MISMATCH,
                  "node memory access width disagrees with TargetModel", node.id});
    if (!target.isMappingResearchTarget() && operation->resultType != node.resultType)
      report.add({TargetDFGDiagnosticCode::TDFG_RESULT_TYPE_MISMATCH,
                  "node result type disagrees with TargetModel", node.id});
    if (!resultRoleMatches(operation->resultRole, node.resultType))
      report.add({TargetDFGDiagnosticCode::TDFG_RESULT_TYPE_MISMATCH,
                  "node result type does not satisfy TargetModel result role", node.id});
    if (isStoreLike(*operation) && node.resultType != ir::ValueType::voidTy())
      report.add({TargetDFGDiagnosticCode::TDFG_STORE_RESULT_INVALID,
                  "target store-like operation must have void result", node.id});
    if (isLoadLike(*operation) &&
        (node.resultType == ir::ValueType::voidTy() || !node.resultLatency))
      report.add({TargetDFGDiagnosticCode::TDFG_LOAD_RESULT_INVALID,
                  "target load-like operation must have a value result and latency", node.id});
    const auto requiredOperands =
        std::count_if(operation->operands.begin(), operation->operands.end(),
                      [](const auto& operand) { return !operand.optional; });
    if (node.operandTypes.size() < static_cast<std::size_t>(requiredOperands) ||
        node.operandTypes.size() > operation->operands.size())
      report.add({TargetDFGDiagnosticCode::TDFG_OPERATION_ARITY_INVALID,
                  "target operation operand count disagrees with TargetModel descriptor", node.id});
    if (!target.supportsValueType(node.resultType))
      report.add({TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE,
                  "node result type is not supported by TargetModel", node.id});
    std::optional<ir::ValueType> uniformDataType;
    for (std::uint32_t operand = 0; operand < node.operandTypes.size(); ++operand) {
      if (!target.supportsValueType(node.operandTypes[operand]))
        report.add({TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE,
                    "node operand type is not supported by TargetModel", node.id, std::nullopt,
                    operand});
      if (operand >= operation->operands.size() ||
          !roleMatches(operation->operands[operand].role, node.operandTypes[operand]) ||
          (operand < operation->operands.size() &&
           !operation->operands[operand].acceptedTypes.empty() &&
           std::find(operation->operands[operand].acceptedTypes.begin(),
                     operation->operands[operand].acceptedTypes.end(),
                     node.operandTypes[operand]) == operation->operands[operand].acceptedTypes.end()))
        report.add({TargetDFGDiagnosticCode::TDFG_OPERATION_OPERAND_INVALID,
                    "node operand type does not satisfy TargetModel operand role", node.id,
                    std::nullopt, operand});
      if (operand < operation->operands.size() && operation->uniformDataType &&
          operation->operands[operand].role == TargetOperandRole::Data) {
        if (uniformDataType && *uniformDataType != node.operandTypes[operand])
          report.add({TargetDFGDiagnosticCode::TDFG_OPERATION_OPERAND_INVALID,
                      "node operands violate TargetModel uniform data type", node.id,
                      std::nullopt, operand});
        uniformDataType = node.operandTypes[operand];
      }
    }
    if (!operation->acceptedResultTypes.empty() &&
        std::find(operation->acceptedResultTypes.begin(), operation->acceptedResultTypes.end(),
                  node.resultType) == operation->acceptedResultTypes.end())
      report.add({TargetDFGDiagnosticCode::TDFG_RESULT_TYPE_MISMATCH,
                  "node result type does not satisfy TargetModel operation type", node.id});
    if (operation->uniformDataType && operation->resultRole == TargetResultRole::Data &&
        uniformDataType && *uniformDataType != node.resultType)
      report.add({TargetDFGDiagnosticCode::TDFG_RESULT_TYPE_MISMATCH,
                  "node result violates TargetModel uniform data type", node.id});
    if (node.genericOrigins.empty())
      report.add({TargetDFGDiagnosticCode::TDFG_PROVENANCE_EMPTY,
                  "Target node must retain Generic provenance", node.id});
    if (generic) {
      for (const auto origin : node.genericOrigins)
        if (!generic->containsNode(origin))
          report.add({TargetDFGDiagnosticCode::TDFG_PROVENANCE_UNKNOWN_GENERIC_NODE,
                      "Target node provenance references unknown Generic node", node.id});
    }
    for (std::uint32_t operand = 0; operand < node.operandTypes.size(); ++operand)
      if (providers[{node.id, operand}] == 0)
        report.add({TargetDFGDiagnosticCode::TDFG_MISSING_PROVIDER,
                    "Target operand has no provider", node.id, std::nullopt, operand});
      else if (providers[{node.id, operand}] > 1)
        report.add({TargetDFGDiagnosticCode::TDFG_DUPLICATE_PROVIDER,
                    "Target operand has multiple providers", node.id, std::nullopt, operand});
  }

  for (const auto& edge : dfg.edges()) {
    const auto srcIt = nodes.find(edge.src);
    const auto dstIt = nodes.find(edge.dst);
    if (srcIt == nodes.end()) {
      report.add({TargetDFGDiagnosticCode::TDFG_UNKNOWN_EDGE_SOURCE, "edge source is unknown",
                  std::nullopt, edge.id});
      continue;
    }
    if (dstIt == nodes.end()) {
      report.add({TargetDFGDiagnosticCode::TDFG_UNKNOWN_EDGE_DESTINATION,
                  "edge destination is unknown", std::nullopt, edge.id});
      continue;
    }
    const auto& src = *srcIt->second;
    const auto& dst = *dstIt->second;
    const auto* srcOperation = target.findOperation(src.operation);
    const auto* dstOperation = target.findOperation(dst.operation);
    if (edge.kind() == ir::Edge::Kind::Data) {
      const auto operand = std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
      if (operand >= dst.operandTypes.size() || !dstOperation ||
          (operand < dstOperation->operands.size() &&
           dstOperation->operands[operand].role != TargetOperandRole::Data &&
           dstOperation->operands[operand].role != TargetOperandRole::Address) ||
          !srcOperation || srcOperation->resultRole != TargetResultRole::Data ||
          (operand < dst.operandTypes.size() && src.resultType != dst.operandTypes[operand]))
        report.add({TargetDFGDiagnosticCode::TDFG_DATA_EDGE_INVALID,
                    "Target data edge has invalid source, operand, or type", dst.id, edge.id,
                    operand});
    } else if (edge.kind() == ir::Edge::Kind::Predicate) {
      const auto operand = std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
      if (operand >= dst.operandTypes.size() || !dstOperation ||
          (operand < dstOperation->operands.size() &&
           dstOperation->operands[operand].role != TargetOperandRole::Predicate) ||
          !srcOperation || srcOperation->resultRole != TargetResultRole::Predicate ||
          !isPredicate(dst.operandTypes[operand]))
        report.add({TargetDFGDiagnosticCode::TDFG_PREDICATE_EDGE_INVALID,
                    "Target predicate edge has invalid source or destination", dst.id, edge.id,
                    operand});
    } else {
      const auto dependence = std::get<ir::MemoryEdgeInfo>(edge.info).dependence;
      const auto* srcOperation = target.findOperation(src.operation);
      const auto* dstOperation = target.findOperation(dst.operation);
      const bool srcStore = srcOperation && isStoreLike(*srcOperation);
      const bool srcLoad = srcOperation && isLoadLike(*srcOperation);
      const bool dstStore = dstOperation && isStoreLike(*dstOperation);
      const bool dstLoad = dstOperation && isLoadLike(*dstOperation);
      const bool valid = (dependence == ir::MemoryDepKind::RAW && srcStore && dstLoad) ||
                         (dependence == ir::MemoryDepKind::WAR && srcLoad && dstStore) ||
                         (dependence == ir::MemoryDepKind::WAW && srcStore && dstStore);
      if (!valid)
        report.add({TargetDFGDiagnosticCode::TDFG_MEMORY_EDGE_INVALID,
                    "Target memory edge has invalid endpoint operations", dst.id, edge.id});
    }
  }
  for (const auto& liveOut : dfg.liveOuts()) {
    const auto it = nodes.find(liveOut.source);
    if (it == nodes.end())
      report.add({TargetDFGDiagnosticCode::TDFG_LIVEOUT_UNKNOWN_NODE,
                  "Target live-out references an unknown node", std::nullopt});
    else if (it->second->resultType != liveOut.type)
      report.add({TargetDFGDiagnosticCode::TDFG_LIVEOUT_TYPE_MISMATCH,
                  "Target live-out type does not match source result", liveOut.source});
    if (!target.supportsValueType(liveOut.type))
      report.add({TargetDFGDiagnosticCode::TDFG_UNSUPPORTED_TYPE,
                  "live-out type is not supported by TargetModel", liveOut.source});
  }

  for (const auto& node : dfg.nodes()) {
    std::vector<TargetEdgeId> expectedIncoming;
    std::vector<TargetEdgeId> expectedOutgoing;
    for (const auto& edge : dfg.edges()) {
      if (edge.dst == node.id && nodes.contains(edge.src) && nodes.contains(edge.dst))
        expectedIncoming.push_back(edge.id);
      if (edge.src == node.id && nodes.contains(edge.src) && nodes.contains(edge.dst))
        expectedOutgoing.push_back(edge.id);
    }
    const auto actualIncoming = dfg.incoming(node.id);
    const auto actualOutgoing = dfg.outgoing(node.id);
    if (!std::equal(actualIncoming.begin(), actualIncoming.end(), expectedIncoming.begin(),
                    expectedIncoming.end()) ||
        !std::equal(actualOutgoing.begin(), actualOutgoing.end(), expectedOutgoing.begin(),
                    expectedOutgoing.end()))
      report.add({TargetDFGDiagnosticCode::TDFG_ADJACENCY_INCONSISTENT,
                  "cached Target DFG adjacency disagrees with edge endpoints", node.id});
  }
  return report;
}

} // namespace cgra::target
