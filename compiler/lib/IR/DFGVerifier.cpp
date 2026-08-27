// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace cgra::ir {

namespace {

using Json = nlohmann::json;

bool isValidType(const ValueType& type) {
  switch (type.kind) {
  case ValueKind::Integer:
  case ValueKind::Float:
    return type.bitWidth != 0;
  case ValueKind::Predicate:
    return type.bitWidth == 1;
  case ValueKind::Void:
    return type.bitWidth == 0;
  }
  return false;
}

bool isDataType(const ValueType& type) {
  return isValidType(type) && (type.kind == ValueKind::Integer || type.kind == ValueKind::Float);
}

bool isIntegerType(const ValueType& type) {
  return isValidType(type) && type.kind == ValueKind::Integer;
}

bool isPredicateType(const ValueType& type) {
  return isValidType(type) && type.kind == ValueKind::Predicate;
}

bool isNonVoidType(const ValueType& type) {
  return isValidType(type) && type.kind != ValueKind::Void;
}

bool isMemoryOpcode(Opcode opcode) { return opcode == Opcode::Load || opcode == Opcode::Store; }

struct Provider {
  enum class Kind { Edge, Binding };
  Kind kind = Kind::Edge;
  EdgeId edge = 0;
  std::size_t binding = 0;
};

template <typename Id, typename Value> using IdIndex = std::unordered_map<Id, const Value*>;

} // namespace

std::string_view toString(DiagnosticSeverity severity) {
  switch (severity) {
  case DiagnosticSeverity::Error:
    return "error";
  case DiagnosticSeverity::Warning:
    return "warning";
  }
  return "unknown";
}

std::string_view toString(DFGDiagnosticCode code) {
  switch (code) {
  case DFGDiagnosticCode::DFG_ID_DUPLICATE_NODE:
    return "DFG_ID_DUPLICATE_NODE";
  case DFGDiagnosticCode::DFG_ID_DUPLICATE_EDGE:
    return "DFG_ID_DUPLICATE_EDGE";
  case DFGDiagnosticCode::DFG_ID_DUPLICATE_EXTERNAL:
    return "DFG_ID_DUPLICATE_EXTERNAL";
  case DFGDiagnosticCode::DFG_ID_DUPLICATE_CONSTANT:
    return "DFG_ID_DUPLICATE_CONSTANT";
  case DFGDiagnosticCode::DFG_ID_DUPLICATE_LIVEOUT:
    return "DFG_ID_DUPLICATE_LIVEOUT";
  case DFGDiagnosticCode::DFG_EDGE_UNKNOWN_SOURCE:
    return "DFG_EDGE_UNKNOWN_SOURCE";
  case DFGDiagnosticCode::DFG_EDGE_UNKNOWN_DESTINATION:
    return "DFG_EDGE_UNKNOWN_DESTINATION";
  case DFGDiagnosticCode::DFG_BINDING_UNKNOWN_NODE:
    return "DFG_BINDING_UNKNOWN_NODE";
  case DFGDiagnosticCode::DFG_BINDING_UNKNOWN_EXTERNAL:
    return "DFG_BINDING_UNKNOWN_EXTERNAL";
  case DFGDiagnosticCode::DFG_BINDING_UNKNOWN_CONSTANT:
    return "DFG_BINDING_UNKNOWN_CONSTANT";
  case DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT:
    return "DFG_ADJACENCY_INCONSISTENT";
  case DFGDiagnosticCode::DFG_OPERAND_INDEX_OUT_OF_RANGE:
    return "DFG_OPERAND_INDEX_OUT_OF_RANGE";
  case DFGDiagnosticCode::DFG_OPERAND_MISSING_PROVIDER:
    return "DFG_OPERAND_MISSING_PROVIDER";
  case DFGDiagnosticCode::DFG_OPERAND_DUPLICATE_PROVIDER:
    return "DFG_OPERAND_DUPLICATE_PROVIDER";
  case DFGDiagnosticCode::DFG_OPERAND_TYPE_MISMATCH:
    return "DFG_OPERAND_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_OPERAND_INVALID_EDGE_KIND:
    return "DFG_OPERAND_INVALID_EDGE_KIND";
  case DFGDiagnosticCode::DFG_TYPE_INVALID:
    return "DFG_TYPE_INVALID";
  case DFGDiagnosticCode::DFG_RESULT_TYPE_INVALID:
    return "DFG_RESULT_TYPE_INVALID";
  case DFGDiagnosticCode::DFG_EDGE_VALUE_TYPE_MISMATCH:
    return "DFG_EDGE_VALUE_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_PREDICATE_EXPECTED:
    return "DFG_PREDICATE_EXPECTED";
  case DFGDiagnosticCode::DFG_DATA_VALUE_EXPECTED:
    return "DFG_DATA_VALUE_EXPECTED";
  case DFGDiagnosticCode::DFG_VOID_VALUE_USED:
    return "DFG_VOID_VALUE_USED";
  case DFGDiagnosticCode::DFG_OPCODE_ARITY_MISMATCH:
    return "DFG_OPCODE_ARITY_MISMATCH";
  case DFGDiagnosticCode::DFG_OPCODE_RESULT_TYPE_MISMATCH:
    return "DFG_OPCODE_RESULT_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_OPCODE_OPERAND_TYPE_MISMATCH:
    return "DFG_OPCODE_OPERAND_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_OPCODE_UNEXPECTED_METADATA:
    return "DFG_OPCODE_UNEXPECTED_METADATA";
  case DFGDiagnosticCode::DFG_OPCODE_MISSING_METADATA:
    return "DFG_OPCODE_MISSING_METADATA";
  case DFGDiagnosticCode::DFG_OPCODE_METADATA_INVALID:
    return "DFG_OPCODE_METADATA_INVALID";
  case DFGDiagnosticCode::DFG_ICMP_MISSING_PREDICATE:
    return "DFG_ICMP_MISSING_PREDICATE";
  case DFGDiagnosticCode::DFG_ICMP_INVALID_RESULT_TYPE:
    return "DFG_ICMP_INVALID_RESULT_TYPE";
  case DFGDiagnosticCode::DFG_ICMP_OPERAND_TYPE_MISMATCH:
    return "DFG_ICMP_OPERAND_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_SELECT_INVALID_PREDICATE:
    return "DFG_SELECT_INVALID_PREDICATE";
  case DFGDiagnosticCode::DFG_SELECT_VALUE_TYPE_MISMATCH:
    return "DFG_SELECT_VALUE_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_SELECT_RESULT_TYPE_MISMATCH:
    return "DFG_SELECT_RESULT_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_LOAD_INVALID_RESULT_TYPE:
    return "DFG_LOAD_INVALID_RESULT_TYPE";
  case DFGDiagnosticCode::DFG_LOAD_INVALID_ADDRESS_TYPE:
    return "DFG_LOAD_INVALID_ADDRESS_TYPE";
  case DFGDiagnosticCode::DFG_STORE_INVALID_RESULT_TYPE:
    return "DFG_STORE_INVALID_RESULT_TYPE";
  case DFGDiagnosticCode::DFG_STORE_INVALID_ADDRESS_TYPE:
    return "DFG_STORE_INVALID_ADDRESS_TYPE";
  case DFGDiagnosticCode::DFG_STORE_INVALID_VALUE_TYPE:
    return "DFG_STORE_INVALID_VALUE_TYPE";
  case DFGDiagnosticCode::DFG_STORE_INVALID_PREDICATE:
    return "DFG_STORE_INVALID_PREDICATE";
  case DFGDiagnosticCode::DFG_DATA_EDGE_INVALID_SOURCE_TYPE:
    return "DFG_DATA_EDGE_INVALID_SOURCE_TYPE";
  case DFGDiagnosticCode::DFG_DATA_EDGE_INVALID_DEST_OPERAND:
    return "DFG_DATA_EDGE_INVALID_DEST_OPERAND";
  case DFGDiagnosticCode::DFG_PRED_EDGE_INVALID_SOURCE_TYPE:
    return "DFG_PRED_EDGE_INVALID_SOURCE_TYPE";
  case DFGDiagnosticCode::DFG_PRED_EDGE_INVALID_DEST_OPERAND:
    return "DFG_PRED_EDGE_INVALID_DEST_OPERAND";
  case DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_SOURCE:
    return "DFG_MEMORY_EDGE_INVALID_SOURCE";
  case DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DESTINATION:
    return "DFG_MEMORY_EDGE_INVALID_DESTINATION";
  case DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DEPENDENCE:
    return "DFG_MEMORY_EDGE_INVALID_DEPENDENCE";
  case DFGDiagnosticCode::DFG_MEMORY_EDGE_UNEXPECTED_OPERAND:
    return "DFG_MEMORY_EDGE_UNEXPECTED_OPERAND";
  case DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_MISSING:
    return "DFG_RECURRENCE_BOUNDARY_MISSING";
  case DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_OFFSET_OUT_OF_RANGE:
    return "DFG_RECURRENCE_BOUNDARY_OFFSET_OUT_OF_RANGE";
  case DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_DUPLICATE_OFFSET:
    return "DFG_RECURRENCE_BOUNDARY_DUPLICATE_OFFSET";
  case DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_SOURCE_UNKNOWN:
    return "DFG_RECURRENCE_BOUNDARY_SOURCE_UNKNOWN";
  case DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_TYPE_MISMATCH:
    return "DFG_RECURRENCE_BOUNDARY_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_LIVEOUT_UNKNOWN_SOURCE:
    return "DFG_LIVEOUT_UNKNOWN_SOURCE";
  case DFGDiagnosticCode::DFG_LIVEOUT_TYPE_MISMATCH:
    return "DFG_LIVEOUT_TYPE_MISMATCH";
  case DFGDiagnosticCode::DFG_LIVEOUT_VOID_SOURCE:
    return "DFG_LIVEOUT_VOID_SOURCE";
  }
  return "DFG_UNKNOWN_DIAGNOSTIC";
}

std::string formatDiagnostic(const DFGDiagnostic& diagnostic) {
  std::ostringstream output;
  output << toString(diagnostic.severity) << " [" << toString(diagnostic.code) << "]";
  if (diagnostic.node)
    output << " node=%n" << *diagnostic.node;
  if (diagnostic.edge)
    output << " edge=%e" << *diagnostic.edge;
  if (diagnostic.operand)
    output << " operand=" << *diagnostic.operand;
  if (diagnostic.external)
    output << " external=%ext" << *diagnostic.external;
  if (diagnostic.constant)
    output << " constant=%c" << *diagnostic.constant;
  if (diagnostic.liveOut)
    output << " live_out=%out" << *diagnostic.liveOut;
  output << ": " << diagnostic.message;
  return output.str();
}

void DFGVerificationReport::add(DFGDiagnostic diagnostic) {
  if (diagnostic.severity == DiagnosticSeverity::Error)
    ++errorCount_;
  else
    ++warningCount_;
  diagnostics_.push_back(std::move(diagnostic));
}

bool DFGVerificationReport::contains(DFGDiagnosticCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string DFGVerificationReport::format() const {
  std::ostringstream output;
  output << (ok() ? "valid" : "invalid") << " DFG: " << errorCount_ << " error(s), "
         << warningCount_ << " warning(s)";
  for (const auto& diagnostic : diagnostics_)
    output << '\n' << formatDiagnostic(diagnostic);
  return output.str();
}

std::string DFGVerificationReport::toJson() const {
  Json root = {{"schema", "cgra.dfg.verification.v1"},
               {"valid", ok()},
               {"errors", errorCount_},
               {"warnings", warningCount_},
               {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics_) {
    Json item = {{"severity", toString(diagnostic.severity)},
                 {"code", toString(diagnostic.code)},
                 {"message", diagnostic.message}};
    if (diagnostic.node)
      item["node"] = *diagnostic.node;
    if (diagnostic.edge)
      item["edge"] = *diagnostic.edge;
    if (diagnostic.operand)
      item["operand"] = *diagnostic.operand;
    if (diagnostic.external)
      item["external"] = *diagnostic.external;
    if (diagnostic.constant)
      item["constant"] = *diagnostic.constant;
    if (diagnostic.liveOut)
      item["live_out"] = *diagnostic.liveOut;
    root["diagnostics"].push_back(std::move(item));
  }
  return root.dump(2) + '\n';
}

void DFGVerificationReport::writeJson(const std::filesystem::path& path) const {
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot write DFG verification report: " + path.string());
  stream << toJson();
}

class DFGVerifierImpl {
public:
  explicit DFGVerifierImpl(const DFG& dfg) : dfg_(dfg) {}

  DFGVerificationReport run() {
    indexValues();
    verifyStructure();
    verifyValueTypes();
    verifyNodes();
    indexProviders();
    verifyBindingsAndProviders();
    verifyEdges();
    verifyLiveOuts();
    verifyAdjacency();
    return std::move(report_);
  }

private:
  template <typename Id, typename Value>
  void indexValues(std::span<const Value> values, IdIndex<Id, Value>& result,
                   DFGDiagnosticCode duplicateCode, std::string_view label) {
    for (const auto& value : values) {
      if (!result.emplace(value.id, &value).second) {
        add(duplicateCode, std::string("duplicate ") + std::string(label) + " ID");
      }
    }
  }

  void indexValues() {
    indexValues<NodeId>(dfg_.nodes(), nodes_, DFGDiagnosticCode::DFG_ID_DUPLICATE_NODE, "node");
    indexValues<EdgeId>(dfg_.edges(), edges_, DFGDiagnosticCode::DFG_ID_DUPLICATE_EDGE, "edge");
    indexValues<ExternalValueId>(dfg_.externalValues(), externals_,
                                 DFGDiagnosticCode::DFG_ID_DUPLICATE_EXTERNAL, "external value");
    indexValues<ConstantId>(dfg_.constants(), constants_,
                            DFGDiagnosticCode::DFG_ID_DUPLICATE_CONSTANT, "constant");
    indexValues<LiveOutId>(dfg_.liveOuts(), liveOuts_, DFGDiagnosticCode::DFG_ID_DUPLICATE_LIVEOUT,
                           "live-out");
  }

  void add(DFGDiagnosticCode code, std::string message, std::optional<NodeId> node = std::nullopt,
           std::optional<EdgeId> edge = std::nullopt,
           std::optional<std::uint32_t> operand = std::nullopt,
           std::optional<ExternalValueId> external = std::nullopt,
           std::optional<ConstantId> constant = std::nullopt,
           std::optional<LiveOutId> liveOut = std::nullopt) {
    report_.add({DiagnosticSeverity::Error, code, std::move(message), node, edge, operand, external,
                 constant, liveOut});
  }

  void verifyStructure() {
    for (const auto& edge : dfg_.edges()) {
      if (!nodes_.contains(edge.src))
        add(DFGDiagnosticCode::DFG_EDGE_UNKNOWN_SOURCE, "edge source does not reference a node",
            std::nullopt, edge.id);
      if (!nodes_.contains(edge.dst))
        add(DFGDiagnosticCode::DFG_EDGE_UNKNOWN_DESTINATION,
            "edge destination does not reference a node", std::nullopt, edge.id);
    }
    for (const auto& binding : dfg_.externalBindings()) {
      if (!nodes_.contains(binding.node))
        add(DFGDiagnosticCode::DFG_BINDING_UNKNOWN_NODE,
            "operand binding does not reference a node", binding.node, std::nullopt,
            binding.operand);
      if (std::holds_alternative<ExternalValueRef>(binding.source)) {
        const auto value = std::get<ExternalValueRef>(binding.source).value;
        if (!externals_.contains(value))
          add(DFGDiagnosticCode::DFG_BINDING_UNKNOWN_EXTERNAL,
              "operand binding does not reference an external value", binding.node, std::nullopt,
              binding.operand, value);
      } else {
        const auto value = std::get<ConstantRef>(binding.source).value;
        if (!constants_.contains(value))
          add(DFGDiagnosticCode::DFG_BINDING_UNKNOWN_CONSTANT,
              "operand binding does not reference a constant", binding.node, std::nullopt,
              binding.operand, std::nullopt, value);
      }
    }
  }

  void verifyValueTypes() {
    for (const auto& value : dfg_.externalValues())
      if (!isValidType(value.type))
        add(DFGDiagnosticCode::DFG_TYPE_INVALID, "external value has an invalid type", std::nullopt,
            std::nullopt, std::nullopt, value.id);
    for (const auto& value : dfg_.constants())
      if (!isValidType(value.type))
        add(DFGDiagnosticCode::DFG_TYPE_INVALID, "constant has an invalid type", std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, value.id);
    for (const auto& node : dfg_.nodes()) {
      if (!isValidType(node.resultType))
        add(DFGDiagnosticCode::DFG_RESULT_TYPE_INVALID, "node has an invalid result type", node.id);
      for (std::uint32_t index = 0; index < node.operandTypes.size(); ++index)
        if (!isValidType(node.operandTypes[index]))
          add(DFGDiagnosticCode::DFG_TYPE_INVALID, "node operand has an invalid type", node.id,
              std::nullopt, index);
    }
  }

  void verifyNodes() {
    for (const auto& node : dfg_.nodes()) {
      switch (node.opcode) {
      case Opcode::Add:
      case Opcode::Sub:
      case Opcode::Mul:
      case Opcode::And:
      case Opcode::Or:
      case Opcode::Xor:
        verifyBinary(node, node.opcode == Opcode::And || node.opcode == Opcode::Or ||
                               node.opcode == Opcode::Xor);
        break;
      case Opcode::Shl:
      case Opcode::LShr:
      case Opcode::AShr:
        verifyShift(node);
        break;
      case Opcode::ICmp:
        verifyICmp(node);
        break;
      case Opcode::Select:
        verifySelect(node);
        break;
      case Opcode::Load:
        verifyLoad(node);
        break;
      case Opcode::Store:
        verifyStore(node);
        break;
      case Opcode::Custom:
        if (!node.operationKey || node.operationKey->empty() || node.operandTypes.empty() ||
            node.resultType == ValueType::voidTy())
          add(DFGDiagnosticCode::DFG_OPCODE_METADATA_INVALID,
              "Custom requires a key, at least one operand, and a value result", node.id);
        break;
      default:
        add(DFGDiagnosticCode::DFG_OPCODE_METADATA_INVALID, "unknown opcode value", node.id);
        break;
      }
      verifyMetadata(node);
    }
  }

  void verifyMetadata(const Node& node) {
    if (node.opcode == Opcode::ICmp) {
      if (!node.icmpPredicate)
        add(DFGDiagnosticCode::DFG_ICMP_MISSING_PREDICATE, "ICmp requires an ICmpPredicate",
            node.id);
    } else if (node.icmpPredicate) {
      add(DFGDiagnosticCode::DFG_OPCODE_UNEXPECTED_METADATA, "only ICmp may carry an ICmpPredicate",
          node.id);
    }

    if (isMemoryOpcode(node.opcode)) {
      if (!node.memoryInfo) {
        add(DFGDiagnosticCode::DFG_OPCODE_MISSING_METADATA, "Load and Store require MemoryOpInfo",
            node.id);
      } else if (node.memoryInfo->accessWidthBits == 0 ||
                 node.memoryInfo->accessWidthBits % 8 != 0) {
        add(DFGDiagnosticCode::DFG_OPCODE_METADATA_INVALID,
            "memory access width must be a non-zero multiple of eight", node.id);
      }
    } else if (node.memoryInfo) {
      add(DFGDiagnosticCode::DFG_OPCODE_UNEXPECTED_METADATA,
          "only Load and Store may carry MemoryOpInfo", node.id);
    }
    if (node.opcode != Opcode::Custom && node.operationKey)
      add(DFGDiagnosticCode::DFG_OPCODE_UNEXPECTED_METADATA,
          "only Custom may carry an operation key", node.id);
    if (node.opcode == Opcode::Custom && (!node.operationKey || node.operationKey->empty()))
      add(DFGDiagnosticCode::DFG_OPCODE_METADATA_INVALID,
          "Custom operation key must not be empty", node.id);
  }

  bool hasArity(const Node& node, std::size_t expected) {
    if (node.operandTypes.size() != expected) {
      add(DFGDiagnosticCode::DFG_OPCODE_ARITY_MISMATCH,
          "opcode has " + std::to_string(node.operandTypes.size()) + " operands; expected " +
              std::to_string(expected),
          node.id);
      return false;
    }
    return true;
  }

  void verifyBinary(const Node& node, bool integerOnly) {
    if (!hasArity(node, 2))
      return;
    const auto& lhs = node.operandTypes[0];
    const auto& rhs = node.operandTypes[1];
    if (!isDataType(lhs) || !isDataType(rhs) || (integerOnly && !isIntegerType(lhs)) ||
        !(lhs == rhs) || !(node.resultType == lhs)) {
      add(DFGDiagnosticCode::DFG_OPCODE_OPERAND_TYPE_MISMATCH,
          "binary operands and result must have the same non-void data type", node.id);
    }
  }

  void verifyShift(const Node& node) {
    if (!hasArity(node, 2))
      return;
    const auto& value = node.operandTypes[0];
    const auto& amount = node.operandTypes[1];
    if (!isIntegerType(value) || !isIntegerType(amount) || !(node.resultType == value))
      add(DFGDiagnosticCode::DFG_OPCODE_OPERAND_TYPE_MISMATCH,
          "shift value, amount, and result must be integer-compatible", node.id);
  }

  void verifyICmp(const Node& node) {
    if (!hasArity(node, 2))
      return;
    if (!isPredicateType(node.resultType))
      add(DFGDiagnosticCode::DFG_ICMP_INVALID_RESULT_TYPE, "ICmp result must have predicate type",
          node.id);
    if (!isIntegerType(node.operandTypes[0]) || !isIntegerType(node.operandTypes[1]) ||
        !(node.operandTypes[0] == node.operandTypes[1]))
      add(DFGDiagnosticCode::DFG_ICMP_OPERAND_TYPE_MISMATCH,
          "ICmp operands must be equal-width integer types", node.id);
  }

  void verifySelect(const Node& node) {
    if (!hasArity(node, 3))
      return;
    if (!isPredicateType(node.operandTypes[0]))
      add(DFGDiagnosticCode::DFG_SELECT_INVALID_PREDICATE, "Select operand 0 must be a predicate",
          node.id, std::nullopt, 0);
    if (!isNonVoidType(node.operandTypes[1]) || !isNonVoidType(node.operandTypes[2]) ||
        !(node.operandTypes[1] == node.operandTypes[2]))
      add(DFGDiagnosticCode::DFG_SELECT_VALUE_TYPE_MISMATCH,
          "Select true and false values must have the same data type", node.id);
    if (!isNonVoidType(node.resultType) || !(node.resultType == node.operandTypes[1]))
      add(DFGDiagnosticCode::DFG_SELECT_RESULT_TYPE_MISMATCH,
          "Select result must match its selected value type", node.id);
  }

  void verifyLoad(const Node& node) {
    if (!hasArity(node, 1))
      return;
    if (!isIntegerType(node.operandTypes[0]))
      add(DFGDiagnosticCode::DFG_LOAD_INVALID_ADDRESS_TYPE, "Load address must be an integer type",
          node.id, std::nullopt, 0);
    if (!isNonVoidType(node.resultType))
      add(DFGDiagnosticCode::DFG_LOAD_INVALID_RESULT_TYPE,
          "Load result must be a non-void data type", node.id);
  }

  void verifyStore(const Node& node) {
    if (node.resultType != ValueType::voidTy())
      add(DFGDiagnosticCode::DFG_STORE_INVALID_RESULT_TYPE, "Store result must be void", node.id);
    if (node.operandTypes.size() < 2 || node.operandTypes.size() > 3) {
      hasArity(node, 2);
      return;
    }
    if (!isIntegerType(node.operandTypes[0]))
      add(DFGDiagnosticCode::DFG_STORE_INVALID_ADDRESS_TYPE,
          "Store address must be an integer type", node.id, std::nullopt, 0);
    if (!isNonVoidType(node.operandTypes[1]))
      add(DFGDiagnosticCode::DFG_STORE_INVALID_VALUE_TYPE,
          "Store value must be a non-void data type", node.id, std::nullopt, 1);
    if (node.operandTypes.size() == 3 && !isPredicateType(node.operandTypes[2]))
      add(DFGDiagnosticCode::DFG_STORE_INVALID_PREDICATE,
          "Store commit predicate must have predicate type", node.id, std::nullopt, 2);
  }

  void indexProviders() {
    for (const auto& edge : dfg_.edges()) {
      if (!nodes_.contains(edge.dst))
        continue;
      if (edge.kind() == Edge::Kind::Memory)
        continue;
      const auto& destination = *nodes_.at(edge.dst);
      const auto operand = edge.kind() == Edge::Kind::Data
                               ? std::get<DataEdgeInfo>(edge.info).dstOperand
                               : std::get<PredicateEdgeInfo>(edge.info).dstOperand;
      if (operand < destination.operandTypes.size())
        providers_[{edge.dst, operand}].push_back({Provider::Kind::Edge, edge.id, 0});
    }
    for (std::size_t index = 0; index < dfg_.externalBindings().size(); ++index) {
      const auto& binding = dfg_.externalBindings()[index];
      if (nodes_.contains(binding.node) &&
          binding.operand < nodes_.at(binding.node)->operandTypes.size())
        providers_[{binding.node, binding.operand}].push_back({Provider::Kind::Binding, 0, index});
    }
  }

  void verifyBindingsAndProviders() {
    for (const auto& binding : dfg_.externalBindings()) {
      const auto nodeIt = nodes_.find(binding.node);
      if (nodeIt == nodes_.end())
        continue;
      if (binding.operand >= nodeIt->second->operandTypes.size()) {
        add(DFGDiagnosticCode::DFG_OPERAND_INDEX_OUT_OF_RANGE,
            "operand binding index is outside the operand list", binding.node, std::nullopt,
            binding.operand);
        continue;
      }
      const auto& expected = nodeIt->second->operandTypes[binding.operand];
      if (std::holds_alternative<ExternalValueRef>(binding.source)) {
        const auto value = std::get<ExternalValueRef>(binding.source).value;
        if (const auto it = externals_.find(value);
            it != externals_.end() && it->second->type != expected)
          add(DFGDiagnosticCode::DFG_OPERAND_TYPE_MISMATCH,
              "external binding type does not match operand type", binding.node, std::nullopt,
              binding.operand, value);
      } else {
        const auto value = std::get<ConstantRef>(binding.source).value;
        if (const auto it = constants_.find(value);
            it != constants_.end() && it->second->type != expected)
          add(DFGDiagnosticCode::DFG_OPERAND_TYPE_MISMATCH,
              "constant binding type does not match operand type", binding.node, std::nullopt,
              binding.operand, std::nullopt, value);
      }
    }
    for (const auto& node : dfg_.nodes()) {
      for (std::uint32_t operand = 0; operand < node.operandTypes.size(); ++operand) {
        const auto it = providers_.find({node.id, operand});
        const auto count = it == providers_.end() ? 0U : it->second.size();
        if (count == 0)
          add(DFGDiagnosticCode::DFG_OPERAND_MISSING_PROVIDER,
              "operand has no data, predicate, external, or constant provider", node.id,
              std::nullopt, operand);
        else if (count > 1)
          add(DFGDiagnosticCode::DFG_OPERAND_DUPLICATE_PROVIDER,
              "operand has more than one value provider", node.id, std::nullopt, operand);
      }
    }
  }

  void verifyEdges() {
    for (const auto& edge : dfg_.edges()) {
      const auto srcIt = nodes_.find(edge.src);
      const auto dstIt = nodes_.find(edge.dst);
      if (srcIt == nodes_.end() || dstIt == nodes_.end())
        continue;
      const auto& source = *srcIt->second;
      const auto& destination = *dstIt->second;
      switch (edge.kind()) {
      case Edge::Kind::Data:
        verifyDataEdge(edge, source, destination);
        break;
      case Edge::Kind::Predicate:
        verifyPredicateEdge(edge, source, destination);
        break;
      case Edge::Kind::Memory:
        verifyMemoryEdge(edge, source, destination);
        break;
      }
    }
  }

  void verifyDataEdge(const Edge& edge, const Node& source, const Node& destination) {
    const auto operand = std::get<DataEdgeInfo>(edge.info).dstOperand;
    if (operand >= destination.operandTypes.size()) {
      add(DFGDiagnosticCode::DFG_OPERAND_INDEX_OUT_OF_RANGE,
          "data edge operand index is outside the operand list", destination.id, edge.id, operand);
      add(DFGDiagnosticCode::DFG_DATA_EDGE_INVALID_DEST_OPERAND,
          "data edge destination operand is outside the operand list", destination.id, edge.id,
          operand);
      return;
    }
    if (source.resultType.kind == ValueKind::Void)
      add(DFGDiagnosticCode::DFG_VOID_VALUE_USED, "void result is used by a data edge", source.id,
          edge.id, operand);
    else if (source.resultType.kind == ValueKind::Predicate)
      add(DFGDiagnosticCode::DFG_DATA_EDGE_INVALID_SOURCE_TYPE,
          "predicate result cannot be carried by a data edge", source.id, edge.id, operand);
    if (destination.operandTypes[operand].kind == ValueKind::Predicate)
      add(DFGDiagnosticCode::DFG_OPERAND_INVALID_EDGE_KIND,
          "predicate operand requires a predicate edge", destination.id, edge.id, operand);
    if (isValidType(source.resultType) && isValidType(destination.operandTypes[operand]) &&
        source.resultType != destination.operandTypes[operand])
      add(DFGDiagnosticCode::DFG_EDGE_VALUE_TYPE_MISMATCH,
          "data edge source type does not match destination operand type", destination.id, edge.id,
          operand);
    verifyBoundary(edge, std::get<DataEdgeInfo>(edge.info).boundary,
                   destination.operandTypes[operand]);
  }

  void verifyPredicateEdge(const Edge& edge, const Node& source, const Node& destination) {
    const auto operand = std::get<PredicateEdgeInfo>(edge.info).dstOperand;
    if (operand >= destination.operandTypes.size()) {
      add(DFGDiagnosticCode::DFG_OPERAND_INDEX_OUT_OF_RANGE,
          "predicate edge operand index is outside the operand list", destination.id, edge.id,
          operand);
      add(DFGDiagnosticCode::DFG_PRED_EDGE_INVALID_DEST_OPERAND,
          "predicate edge destination operand is outside the operand list", destination.id, edge.id,
          operand);
      return;
    }
    if (!isPredicateType(source.resultType))
      add(DFGDiagnosticCode::DFG_PRED_EDGE_INVALID_SOURCE_TYPE,
          "predicate edge source must produce a predicate", source.id, edge.id, operand);
    if (!isPredicateType(destination.operandTypes[operand]))
      add(DFGDiagnosticCode::DFG_OPERAND_INVALID_EDGE_KIND, "data operand requires a data edge",
          destination.id, edge.id, operand);
    if (isValidType(source.resultType) && isValidType(destination.operandTypes[operand]) &&
        source.resultType != destination.operandTypes[operand])
      add(DFGDiagnosticCode::DFG_EDGE_VALUE_TYPE_MISMATCH,
          "predicate edge source type does not match destination operand type", destination.id,
          edge.id, operand);
    verifyBoundary(edge, std::get<PredicateEdgeInfo>(edge.info).boundary,
                   destination.operandTypes[operand]);
  }

  void verifyBoundary(const Edge& edge, const std::optional<RecurrenceBoundary>& boundary,
                      const ValueType& expectedType) {
    if (edge.distance == 0) {
      if (boundary)
        add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_OFFSET_OUT_OF_RANGE,
            "distance-zero edge cannot carry recurrence boundary values", std::nullopt, edge.id);
      return;
    }
    if (!boundary || boundary->values.size() != edge.distance) {
      add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_MISSING,
          "loop-carried edge requires one boundary value for every pre-seed iteration",
          std::nullopt, edge.id);
      return;
    }
    std::set<std::uint32_t> offsets;
    for (const auto& value : boundary->values) {
      if (value.iterationOffset >= edge.distance) {
        add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_OFFSET_OUT_OF_RANGE,
            "recurrence boundary iteration offset is outside the edge distance", std::nullopt,
            edge.id);
        continue;
      }
      if (!offsets.insert(value.iterationOffset).second)
        add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_DUPLICATE_OFFSET,
            "recurrence boundary repeats an iteration offset", std::nullopt, edge.id);
      const ValueType* provided = nullptr;
      if (std::holds_alternative<ExternalValueRef>(value.value)) {
        const auto id = std::get<ExternalValueRef>(value.value).value;
        const auto it = externals_.find(id);
        if (it == externals_.end())
          add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_SOURCE_UNKNOWN,
              "recurrence boundary references an unknown external value", std::nullopt, edge.id,
              std::nullopt, id);
        else
          provided = &it->second->type;
      } else {
        const auto id = std::get<ConstantRef>(value.value).value;
        const auto it = constants_.find(id);
        if (it == constants_.end())
          add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_SOURCE_UNKNOWN,
              "recurrence boundary references an unknown constant", std::nullopt, edge.id,
              std::nullopt, std::nullopt, id);
        else
          provided = &it->second->type;
      }
      if (provided && *provided != expectedType)
        add(DFGDiagnosticCode::DFG_RECURRENCE_BOUNDARY_TYPE_MISMATCH,
            "recurrence boundary value type does not match destination operand", std::nullopt,
            edge.id);
    }
  }

  void verifyMemoryEdge(const Edge& edge, const Node& source, const Node& destination) {
    const auto dependence = std::get<MemoryEdgeInfo>(edge.info).dependence;
    switch (dependence) {
    case MemoryDepKind::RAW:
      if (source.opcode != Opcode::Store)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_SOURCE,
            "RAW memory edge source must be Store", source.id, edge.id);
      if (destination.opcode != Opcode::Load)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DESTINATION,
            "RAW memory edge destination must be Load", destination.id, edge.id);
      break;
    case MemoryDepKind::WAR:
      if (source.opcode != Opcode::Load)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_SOURCE,
            "WAR memory edge source must be Load", source.id, edge.id);
      if (destination.opcode != Opcode::Store)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DESTINATION,
            "WAR memory edge destination must be Store", destination.id, edge.id);
      break;
    case MemoryDepKind::WAW:
      if (source.opcode != Opcode::Store)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_SOURCE,
            "WAW memory edge source must be Store", source.id, edge.id);
      if (destination.opcode != Opcode::Store)
        add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DESTINATION,
            "WAW memory edge destination must be Store", destination.id, edge.id);
      break;
    default:
      add(DFGDiagnosticCode::DFG_MEMORY_EDGE_INVALID_DEPENDENCE,
          "memory edge has an unknown dependence kind", std::nullopt, edge.id);
      break;
    }
  }

  void verifyLiveOuts() {
    for (const auto& liveOut : dfg_.liveOuts()) {
      if (!isValidType(liveOut.type))
        add(DFGDiagnosticCode::DFG_TYPE_INVALID, "live-out has an invalid type", std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, liveOut.id);
      const auto it = nodes_.find(liveOut.source);
      if (it == nodes_.end()) {
        add(DFGDiagnosticCode::DFG_LIVEOUT_UNKNOWN_SOURCE,
            "live-out source does not reference a node", std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, liveOut.id);
        continue;
      }
      if (it->second->resultType.kind == ValueKind::Void)
        add(DFGDiagnosticCode::DFG_LIVEOUT_VOID_SOURCE, "live-out cannot expose a void node result",
            liveOut.source, std::nullopt, std::nullopt, std::nullopt, std::nullopt, liveOut.id);
      if (isValidType(it->second->resultType) && isValidType(liveOut.type) &&
          it->second->resultType != liveOut.type)
        add(DFGDiagnosticCode::DFG_LIVEOUT_TYPE_MISMATCH,
            "live-out type does not match its source node result", liveOut.source, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, liveOut.id);
    }
  }

  void verifyAdjacency() {
    std::map<NodeId, std::vector<EdgeId>> expectedIncoming;
    std::map<NodeId, std::vector<EdgeId>> expectedOutgoing;
    for (const auto& node : dfg_.nodes()) {
      expectedIncoming[node.id];
      expectedOutgoing[node.id];
      try {
        if (!(dfg_.node(node.id) == node))
          add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
              "node ID index does not resolve to the stored node", node.id);
      } catch (const std::exception&) {
        add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
            "node ID index cannot resolve the stored node", node.id);
      }
    }
    for (const auto& edge : dfg_.edges()) {
      if (nodes_.contains(edge.src))
        expectedOutgoing[edge.src].push_back(edge.id);
      if (nodes_.contains(edge.dst))
        expectedIncoming[edge.dst].push_back(edge.id);
      try {
        if (!(dfg_.edge(edge.id) == edge))
          add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
              "edge ID index does not resolve to the stored edge", std::nullopt, edge.id);
      } catch (const std::exception&) {
        add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
            "edge ID index cannot resolve the stored edge", std::nullopt, edge.id);
      }
    }
    for (const auto& [node, expected] : expectedIncoming) {
      try {
        const auto actual = dfg_.incoming(node);
        if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()))
          add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
              "cached incoming adjacency does not match graph edges", node);
      } catch (const std::exception&) {
        add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
            "cached incoming adjacency is not readable", node);
      }
    }
    for (const auto& [node, expected] : expectedOutgoing) {
      try {
        const auto actual = dfg_.outgoing(node);
        if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()))
          add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
              "cached outgoing adjacency does not match graph edges", node);
      } catch (const std::exception&) {
        add(DFGDiagnosticCode::DFG_ADJACENCY_INCONSISTENT,
            "cached outgoing adjacency is not readable", node);
      }
    }
  }

  const DFG& dfg_;
  DFGVerificationReport report_;
  IdIndex<NodeId, Node> nodes_;
  IdIndex<EdgeId, Edge> edges_;
  IdIndex<ExternalValueId, ExternalValue> externals_;
  IdIndex<ConstantId, ConstantValue> constants_;
  IdIndex<LiveOutId, LiveOut> liveOuts_;
  std::map<std::pair<NodeId, std::uint32_t>, std::vector<Provider>> providers_;
};

DFGVerificationReport DFGVerifier::verify(const DFG& dfg) { return DFGVerifierImpl(dfg).run(); }

} // namespace cgra::ir
