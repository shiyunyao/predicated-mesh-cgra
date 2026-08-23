// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace cgra::ir {

class DFGBuilder {
public:
  explicit DFGBuilder(std::string name = {}) : dfg_(std::move(name)) {}

  ExternalValueId addExternal(std::string name, ValueType type);
  ExternalValueId importExternal(ExternalValue value) {
    nextExternalId_ = std::max(nextExternalId_, value.id + 1);
    return dfg_.appendExternal(std::move(value));
  }
  ConstantId addConstant(ValueType type, std::uint64_t bits);
  ConstantId importConstant(ConstantValue value) {
    nextConstantId_ = std::max(nextConstantId_, value.id + 1);
    return dfg_.appendConstant(std::move(value));
  }
  LiveOutId addLiveOut(std::string name, ValueType type, NodeId source);
  NodeId addNode(Opcode opcode, std::vector<ValueType> operandTypes, ValueType resultType,
                 std::optional<ICmpPredicate> predicate = std::nullopt,
                 std::optional<MemoryOpInfo> memoryInfo = std::nullopt,
                 std::optional<SourceInfo> source = std::nullopt);
  NodeId importNode(Node node) {
    nextNodeId_ = std::max(nextNodeId_, node.id + 1);
    return dfg_.appendNode(std::move(node));
  }

  void bindExternal(NodeId node, std::uint32_t operand, ExternalValueId value);
  void bindConstant(NodeId node, std::uint32_t operand, ConstantId value);
  EdgeId addDataEdge(NodeId src, NodeId dst, std::uint32_t dstOperand, std::uint32_t distance = 0);
  EdgeId addDataEdge(NodeId src, NodeId dst, std::uint32_t dstOperand, std::uint32_t distance,
                     std::optional<RecurrenceBoundary> boundary);
  EdgeId addPredicateEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                          std::uint32_t distance = 0);
  EdgeId addPredicateEdge(NodeId src, NodeId dst, std::uint32_t dstOperand, std::uint32_t distance,
                          std::optional<RecurrenceBoundary> boundary);
  EdgeId addMemoryEdge(NodeId src, NodeId dst, MemoryDepKind dependence,
                       std::uint32_t distance = 0);
  LiveOutId importLiveOut(LiveOut liveOut) {
    nextLiveOutId_ = std::max(nextLiveOutId_, liveOut.id + 1);
    return dfg_.appendLiveOut(std::move(liveOut));
  }
  EdgeId importEdge(Edge edge) {
    nextEdgeId_ = std::max(nextEdgeId_, edge.id + 1);
    return dfg_.appendEdge(std::move(edge));
  }

  DFG finish() { return std::move(dfg_); }

private:
  void checkNode(NodeId id) const;
  void checkOperand(NodeId node, std::uint32_t operand) const;
  void checkProviderAvailable(NodeId node, std::uint32_t operand) const;

  DFG dfg_;
  NodeId nextNodeId_ = 0;
  EdgeId nextEdgeId_ = 0;
  ExternalValueId nextExternalId_ = 0;
  ConstantId nextConstantId_ = 0;
  LiveOutId nextLiveOutId_ = 0;
};

} // namespace cgra::ir
