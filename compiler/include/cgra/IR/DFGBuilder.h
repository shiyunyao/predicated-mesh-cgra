// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <optional>
#include <utility>

namespace cgra::ir {

class DFGBuilder {
public:
  explicit DFGBuilder(std::string name = {}) : dfg_(std::move(name)) {}

  ExternalValueId addExternal(std::string name, ValueType type);
  ConstantId addConstant(ValueType type, std::uint64_t bits);
  LiveOutId addLiveOut(std::string name, ValueType type, NodeId source);
  NodeId addNode(Opcode opcode, std::vector<ValueType> operandTypes, ValueType resultType,
                 std::optional<ICmpPredicate> predicate = std::nullopt,
                 std::optional<MemoryOpInfo> memoryInfo = std::nullopt,
                 std::optional<SourceInfo> source = std::nullopt);

  void bindExternal(NodeId node, std::uint32_t operand, ExternalValueId value);
  void bindConstant(NodeId node, std::uint32_t operand, ConstantId value);
  EdgeId addDataEdge(NodeId src, NodeId dst, std::uint32_t dstOperand, std::uint32_t distance = 0);
  EdgeId addPredicateEdge(NodeId src, NodeId dst, std::uint32_t dstOperand,
                          std::uint32_t distance = 0);
  EdgeId addMemoryEdge(NodeId src, NodeId dst, MemoryDepKind dependence,
                       std::uint32_t distance = 0);

  DFG finish() { return std::move(dfg_); }

private:
  void checkNode(NodeId id) const;
  void checkOperand(NodeId node, std::uint32_t operand) const;
  void checkProviderAvailable(NodeId node, std::uint32_t operand) const;

  DFG dfg_;
};

} // namespace cgra::ir
