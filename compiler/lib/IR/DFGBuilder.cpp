// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::ir {

ExternalValueId DFGBuilder::addExternal(std::string name, ValueType type) {
  if (name.empty())
    throw std::invalid_argument("DFG external value name must not be empty");
  return dfg_.appendExternal(
      {static_cast<ExternalValueId>(dfg_.externalValues().size()), type, std::move(name)});
}

ConstantId DFGBuilder::addConstant(ValueType type, std::uint64_t bits) {
  return dfg_.appendConstant({static_cast<ConstantId>(dfg_.constants().size()), type, bits});
}

LiveOutId DFGBuilder::addLiveOut(std::string name, ValueType type, NodeId source) {
  if (name.empty())
    throw std::invalid_argument("DFG live-out name must not be empty");
  checkNode(source);
  return dfg_.appendLiveOut(
      {static_cast<LiveOutId>(dfg_.liveOuts().size()), type, std::move(name), source});
}

NodeId DFGBuilder::addNode(Opcode opcode, std::vector<ValueType> operandTypes, ValueType resultType,
                           std::optional<ICmpPredicate> predicate,
                           std::optional<MemoryOpInfo> memoryInfo,
                           std::optional<SourceInfo> source) {
  Node node{static_cast<NodeId>(dfg_.nodes().size()),
            opcode,
            resultType,
            std::move(operandTypes),
            predicate,
            memoryInfo,
            std::move(source)};
  return dfg_.appendNode(std::move(node));
}

void DFGBuilder::checkNode(NodeId id) const { static_cast<void>(dfg_.node(id)); }

void DFGBuilder::checkOperand(NodeId node, std::uint32_t operand) const {
  checkNode(node);
  if (operand >= dfg_.node(node).operandTypes.size())
    throw std::out_of_range("DFG operand index is outside the node operand list");
}

void DFGBuilder::checkProviderAvailable(NodeId node, std::uint32_t operand) const {
  checkOperand(node, operand);
  const auto& incoming = dfg_.incoming(node);
  for (const auto edgeId : incoming) {
    const auto& edge = dfg_.edge(edgeId);
    if (edge.kind() == Edge::Kind::Data) {
      if (std::get<DataEdgeInfo>(edge.info).dstOperand == operand)
        throw std::invalid_argument("DFG operand already has a value provider");
    } else if (edge.kind() == Edge::Kind::Predicate) {
      if (std::get<PredicateEdgeInfo>(edge.info).dstOperand == operand)
        throw std::invalid_argument("DFG operand already has a value provider");
    }
  }
  for (const auto& binding : dfg_.externalBindings()) {
    if (binding.node == node && binding.operand == operand)
      throw std::invalid_argument("DFG operand already has an external provider");
  }
}

void DFGBuilder::bindExternal(NodeId node, std::uint32_t operand, ExternalValueId value) {
  checkProviderAvailable(node, operand);
  if (!dfg_.containsExternal(value))
    throw std::out_of_range("DFG external binding references unknown value");
  dfg_.appendBinding({node, operand, ExternalValueRef{value}});
}

void DFGBuilder::bindConstant(NodeId node, std::uint32_t operand, ConstantId value) {
  checkProviderAvailable(node, operand);
  if (!dfg_.containsConstant(value))
    throw std::out_of_range("DFG constant binding references unknown value");
  dfg_.appendBinding({node, operand, ConstantRef{value}});
}

EdgeId DFGBuilder::addDataEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                               std::uint32_t distance) {
  return addDataEdge(src, dst, dstOperand, distance, std::nullopt);
}

EdgeId DFGBuilder::addDataEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                               std::uint32_t distance, std::optional<RecurrenceBoundary> boundary) {
  checkProviderAvailable(dst, dstOperand);
  return dfg_.appendEdge({static_cast<EdgeId>(dfg_.edges().size()), src, dst, distance,
                          DataEdgeInfo{dstOperand, std::move(boundary)}});
}

EdgeId DFGBuilder::addPredicateEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                                    std::uint32_t distance) {
  return addPredicateEdge(src, dst, dstOperand, distance, std::nullopt);
}

EdgeId DFGBuilder::addPredicateEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                                    std::uint32_t distance,
                                    std::optional<RecurrenceBoundary> boundary) {
  checkProviderAvailable(dst, dstOperand);
  return dfg_.appendEdge({static_cast<EdgeId>(dfg_.edges().size()), src, dst, distance,
                          PredicateEdgeInfo{dstOperand, std::move(boundary)}});
}

EdgeId DFGBuilder::addMemoryEdge(NodeId src, NodeId dst, MemoryDepKind dependence,
                                 std::uint32_t distance) {
  checkNode(src);
  checkNode(dst);
  return dfg_.appendEdge(
      {static_cast<EdgeId>(dfg_.edges().size()), src, dst, distance, MemoryEdgeInfo{dependence}});
}

} // namespace cgra::ir
