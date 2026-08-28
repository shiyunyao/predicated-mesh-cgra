// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace cgra::target {
namespace {

using Json = nlohmann::json;

std::string operationFor(const ir::Node& node) {
  switch (node.opcode) {
  case ir::Opcode::Add:
    return "ADD";
  case ir::Opcode::Sub:
    return "SUB";
  case ir::Opcode::Mul:
    return "MUL";
  case ir::Opcode::And:
    return "AND";
  case ir::Opcode::Or:
    return "OR";
  case ir::Opcode::Xor:
    return "XOR";
  case ir::Opcode::Shl:
    return "SHL";
  case ir::Opcode::LShr:
    return "LSHR";
  case ir::Opcode::AShr:
    return "ASHR";
  case ir::Opcode::Select:
    return "SELECT";
  case ir::Opcode::Load:
    return "LOAD";
  case ir::Opcode::Store:
    return "STORE";
  case ir::Opcode::ICmp:
    if (!node.icmpPredicate)
      return {};
    switch (*node.icmpPredicate) {
    case ir::ICmpPredicate::EQ:
      return "CMP_EQ";
    case ir::ICmpPredicate::NE:
      return "CMP_NE";
    case ir::ICmpPredicate::ULT:
      return "CMP_ULT";
    case ir::ICmpPredicate::ULE:
      return "CMP_ULE";
    case ir::ICmpPredicate::UGT:
      return "CMP_UGT";
    case ir::ICmpPredicate::UGE:
      return "CMP_UGE";
    case ir::ICmpPredicate::SLT:
      return "CMP_SLT";
    case ir::ICmpPredicate::SLE:
      return "CMP_SLE";
    case ir::ICmpPredicate::SGT:
      return "CMP_SGT";
    case ir::ICmpPredicate::SGE:
      return "CMP_SGE";
    }
    return {};
  case ir::Opcode::Custom:
    return node.operationKey.value_or("");
  }
  return {};
}

std::string opcodeName(const ir::Node& node) {
  return node.opcode == ir::Opcode::Custom && node.operationKey
             ? "Custom(" + *node.operationKey + ")"
             : std::string(ir::toString(node.opcode));
}

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

void addDiagnostic(TargetLegalizationResult& result, LegalizationStatus status, std::string code,
                   std::string message, std::optional<ir::NodeId> node = std::nullopt) {
  result.diagnostics.push_back({status, std::move(code), std::move(message), node});
}

} // namespace

std::string_view toString(LegalizationStatus status) {
  switch (status) {
  case LegalizationStatus::Success:
    return "success";
  case LegalizationStatus::InvalidGenericDFG:
    return "invalid_generic_dfg";
  case LegalizationStatus::UnsupportedType:
    return "unsupported_type";
  case LegalizationStatus::UnsupportedOperation:
    return "unsupported_operation";
  case LegalizationStatus::UnsupportedComparePredicate:
    return "unsupported_compare_predicate";
  case LegalizationStatus::UnsupportedMemoryAccessWidth:
    return "unsupported_memory_access_width";
  case LegalizationStatus::NoCompatibleExecutionResource:
    return "no_compatible_execution_resource";
  case LegalizationStatus::TargetContractError:
    return "target_contract_error";
  case LegalizationStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string TargetLegalizationResult::format() const {
  std::ostringstream output;
  output << (ok() ? "legalization succeeded" : "legalization failed");
  for (const auto& diagnostic : diagnostics) {
    output << '\n' << diagnostic.code;
    if (diagnostic.genericNode)
      output << " node=%n" << *diagnostic.genericNode;
    output << ": " << diagnostic.message;
  }
  return output.str();
}

std::string TargetLegalizationResult::toJson() const {
  Json root = {
      {"schema", "cgra.target_legalization.v1"}, {"valid", ok()}, {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics) {
    Json item = {{"status", toString(diagnostic.status)},
                 {"code", diagnostic.code},
                 {"message", diagnostic.message}};
    if (diagnostic.genericNode)
      item["generic_node"] = *diagnostic.genericNode;
    root["diagnostics"].push_back(std::move(item));
  }
  return root.dump(2) + '\n';
}

TargetLegalizationResult TargetLegalizer::legalize(const ir::DFG& generic,
                                                   const TargetModel& target) {
  TargetLegalizationResult result;
  const auto genericReport = ir::DFGVerifier::verify(generic);
  if (!genericReport.ok()) {
    addDiagnostic(result, LegalizationStatus::InvalidGenericDFG, "TLEG_INVALID_GENERIC_DFG",
                  genericReport.format());
    return result;
  }

  struct Plan {
    ir::NodeId genericNode = 0;
    const TargetOperationDesc* operation = nullptr;
  };
  std::vector<Plan> plan;
  plan.reserve(generic.nodes().size());

  for (const auto& value : generic.externalValues()) {
    if (!target.supportsValueType(value.type))
      addDiagnostic(result, LegalizationStatus::UnsupportedType, "TLEG_UNSUPPORTED_TYPE",
                    "external value type " + value.type.toString() + " is not supported by target");
  }
  for (const auto& value : generic.constants()) {
    if (!target.supportsValueType(value.type))
      addDiagnostic(result, LegalizationStatus::UnsupportedType, "TLEG_UNSUPPORTED_TYPE",
                    "constant type " + value.type.toString() + " is not supported by target");
  }

  for (const auto& node : generic.nodes()) {
    bool supportedType = target.supportsValueType(node.resultType);
    for (const auto& operand : node.operandTypes)
      supportedType = target.supportsValueType(operand) && supportedType;
    if (!supportedType) {
      addDiagnostic(result, LegalizationStatus::UnsupportedType, "TLEG_UNSUPPORTED_TYPE",
                    "node " + opcodeName(node) + " uses a type unsupported by target", node.id);
      continue;
    }

    auto operationName = operationFor(node);
    if (target.isMappingResearchTarget() && node.memoryInfo &&
        (node.opcode == ir::Opcode::Load || node.opcode == ir::Opcode::Store)) {
      const auto widthSpecific = operationName + std::to_string(node.memoryInfo->accessWidthBits);
      if (target.findOperation(widthSpecific))
        operationName = widthSpecific;
    }
    if (operationName.empty()) {
      const auto code = node.opcode == ir::Opcode::ICmp ? "TLEG_UNSUPPORTED_ICMP_PREDICATE"
                                                        : "TLEG_UNSUPPORTED_OPCODE";
      addDiagnostic(result,
                    node.opcode == ir::Opcode::ICmp
                        ? LegalizationStatus::UnsupportedComparePredicate
                        : LegalizationStatus::UnsupportedOperation,
                    code, "target has no one-to-one operation for " + opcodeName(node), node.id);
      continue;
    }
    const auto* operation = target.findOperation(operationName);
    if (!operation) {
      const bool compare = node.opcode == ir::Opcode::ICmp;
      addDiagnostic(result,
                    compare ? LegalizationStatus::UnsupportedComparePredicate
                            : LegalizationStatus::UnsupportedOperation,
                    compare ? "TLEG_UNSUPPORTED_ICMP_PREDICATE" : "TLEG_MISSING_TARGET_OPERATION",
                    "target does not provide operation " + operationName, node.id);
      continue;
    }
    if (!target.isOperationExecutable(*operation)) {
      addDiagnostic(result, LegalizationStatus::NoCompatibleExecutionResource,
                    "TLEG_NO_COMPATIBLE_EXECUTION_RESOURCE",
                    "target operation " + operationName + " has no compatible execution resource",
                    node.id);
      continue;
    }
    if ((node.opcode == ir::Opcode::Load || node.opcode == ir::Opcode::Store) &&
        (!node.memoryInfo || !operation->accessWidthBits ||
         node.memoryInfo->accessWidthBits != *operation->accessWidthBits)) {
      addDiagnostic(result, LegalizationStatus::UnsupportedMemoryAccessWidth,
                    "TLEG_UNSUPPORTED_MEMORY_ACCESS_WIDTH",
                    "memory operation width is not supported by target", node.id);
      continue;
    }
    const auto requiredOperands = static_cast<std::size_t>(
        std::count_if(operation->operands.begin(), operation->operands.end(),
                      [](const auto& operand) { return !operand.optional; }));
    if (node.operandTypes.size() < static_cast<std::size_t>(requiredOperands) ||
        node.operandTypes.size() > operation->operands.size()) {
      addDiagnostic(
          result, LegalizationStatus::TargetContractError, "TLEG_INVALID_TARGET_OPERATION_DESC",
          "target operation " + operationName + " has an incompatible operand count", node.id);
      continue;
    }
    bool operandSignatureValid = true;
    for (std::size_t operand = 0; operand < node.operandTypes.size(); ++operand) {
      const auto& descriptor = operation->operands[operand];
      if (!roleMatches(descriptor.role, node.operandTypes[operand]) ||
          (descriptor.type && *descriptor.type != node.operandTypes[operand])) {
        operandSignatureValid = false;
        break;
      }
    }
    if (!operandSignatureValid || !resultRoleMatches(operation->resultRole, node.resultType) ||
        (operation->declaredResultType && *operation->declaredResultType != node.resultType)) {
      addDiagnostic(
          result, LegalizationStatus::TargetContractError, "TLEG_INVALID_TARGET_OPERATION_DESC",
          "target operation " + operationName + " semantic signature does not match Generic DFG",
          node.id);
      continue;
    }
    if (!target.isMappingResearchTarget() && operation->resultType != node.resultType) {
      addDiagnostic(
          result, LegalizationStatus::TargetContractError, "TLEG_INVALID_TARGET_OPERATION_DESC",
          "target operation " + operationName + " result type does not match Generic DFG", node.id);
      continue;
    }
    plan.push_back({node.id, operation});
  }

  for (const auto& value : generic.liveOuts()) {
    if (!target.supportsValueType(value.type))
      addDiagnostic(result, LegalizationStatus::UnsupportedType, "TLEG_UNSUPPORTED_TYPE",
                    "live-out type " + value.type.toString() + " is not supported by target",
                    value.source);
  }

  if (!result.diagnostics.empty())
    return result;

  // Materialize only after the complete preflight succeeds. IDs are allocated independently
  // from Generic node IDs, while the map remains one-to-many for future legalization patterns.
  TargetDFGBuilder materialized(generic.name(), std::string(target.name()));
  for (const auto& value : generic.externalValues())
    materialized.addExternal(value);
  for (const auto& value : generic.constants())
    materialized.addConstant(value);

  TargetNodeId nextNode = 0;
  for (const auto& item : plan) {
    const auto& source = generic.node(item.genericNode);
    const auto& operation = *item.operation;
    const auto targetId = materialized.addNode({nextNode++,
                                                operation.id,
                                                operation.executionClass,
                                                source.resultType,
                                                source.operandTypes,
                                                operation.issueOccupancy,
                                                operation.resultLatency,
                                                {source.id},
                                                operation.producerOutputReadyOffset,
                                                operation.accessWidthBits});
    result.map.genericToTarget[item.genericNode].push_back(targetId);
  }
  TargetEdgeId nextEdge = 0;
  for (const auto& edge : generic.edges()) {
    const auto src = result.map.genericToTarget.at(edge.src).front();
    const auto dst = result.map.genericToTarget.at(edge.dst).front();
    materialized.addEdge({nextEdge++, src, dst, edge.distance, edge.info});
  }
  for (const auto& binding : generic.externalBindings()) {
    materialized.addBinding(
        {result.map.genericToTarget.at(binding.node).front(), binding.operand, binding.source});
  }
  for (const auto& liveOut : generic.liveOuts())
    materialized.addLiveOut({liveOut.id, liveOut.type, liveOut.name,
                             result.map.genericToTarget.at(liveOut.source).front()});
  result.dfg = materialized.finish();
  return result;
}

} // namespace cgra::target
