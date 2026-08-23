// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"
#include "cgra/Target/TargetOperation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cgra::target {

using TargetNodeId = std::uint32_t;
using TargetEdgeId = std::uint32_t;

struct TargetNode {
  TargetNodeId id = 0;
  TargetOperationRef operation;
  TargetExecutionClass executionClass = TargetExecutionClass::FU;
  ir::ValueType resultType = ir::ValueType::voidTy();
  std::vector<ir::ValueType> operandTypes;
  unsigned issueOccupancy = 1;
  std::optional<unsigned> resultLatency;
  std::vector<ir::NodeId> genericOrigins;

  friend bool operator==(const TargetNode&, const TargetNode&) = default;
};

struct TargetLiveOut {
  ir::LiveOutId id = 0;
  ir::ValueType type = ir::ValueType::voidTy();
  std::string name;
  TargetNodeId source = 0;

  friend bool operator==(const TargetLiveOut&, const TargetLiveOut&) = default;
};

struct TargetOperandBinding {
  TargetNodeId node = 0;
  std::uint32_t operand = 0;
  ir::ExternalOperandBinding source = ir::ExternalValueRef{};

  friend bool operator==(const TargetOperandBinding&, const TargetOperandBinding&) = default;
};

struct TargetEdge {
  TargetEdgeId id = 0;
  TargetNodeId src = 0;
  TargetNodeId dst = 0;
  std::uint32_t distance = 0;
  ir::EdgeInfo info = ir::DataEdgeInfo{};

  ir::Edge::Kind kind() const noexcept {
    switch (info.index()) {
    case 0:
      return ir::Edge::Kind::Data;
    case 1:
      return ir::Edge::Kind::Predicate;
    default:
      return ir::Edge::Kind::Memory;
    }
  }

  friend bool operator==(const TargetEdge&, const TargetEdge&) = default;
};

struct LegalizationMap {
  std::map<ir::NodeId, std::vector<TargetNodeId>> genericToTarget;

  friend bool operator==(const LegalizationMap&, const LegalizationMap&) = default;
};

class TargetDFGBuilder;
class TargetDFGTestAccess;

class TargetDFG {
public:
  explicit TargetDFG(std::string name = {}, std::string targetName = {});

  const std::string& name() const noexcept { return name_; }
  const std::string& targetName() const noexcept { return targetName_; }
  std::span<const TargetNode> nodes() const noexcept { return nodes_; }
  std::span<const TargetEdge> edges() const noexcept { return edges_; }
  std::span<const ir::ExternalValue> externalValues() const noexcept { return externalValues_; }
  std::span<const ir::ConstantValue> constants() const noexcept { return constants_; }
  std::span<const TargetLiveOut> liveOuts() const noexcept { return liveOuts_; }
  std::span<const TargetOperandBinding> operandBindings() const noexcept { return bindings_; }

  bool containsNode(TargetNodeId id) const noexcept;
  bool containsEdge(TargetEdgeId id) const noexcept;
  const TargetNode& node(TargetNodeId id) const;
  const TargetEdge& edge(TargetEdgeId id) const;
  std::span<const TargetEdgeId> incoming(TargetNodeId id) const;
  std::span<const TargetEdgeId> outgoing(TargetNodeId id) const;

  bool operator==(const TargetDFG& other) const noexcept;

private:
  friend class TargetDFGBuilder;
  friend class TargetDFGTestAccess;

  TargetNodeId appendNode(TargetNode node);
  TargetEdgeId appendEdge(TargetEdge edge);
  void appendExternal(ir::ExternalValue value);
  void appendConstant(ir::ConstantValue value);
  void appendLiveOut(TargetLiveOut liveOut);
  void appendBinding(TargetOperandBinding binding);

  std::string name_;
  std::string targetName_;
  std::vector<TargetNode> nodes_;
  std::vector<TargetEdge> edges_;
  std::vector<ir::ExternalValue> externalValues_;
  std::vector<ir::ConstantValue> constants_;
  std::vector<TargetLiveOut> liveOuts_;
  std::vector<TargetOperandBinding> bindings_;
  std::vector<std::vector<TargetEdgeId>> incoming_;
  std::vector<std::vector<TargetEdgeId>> outgoing_;
  std::unordered_map<TargetNodeId, std::size_t> nodeIndices_;
  std::unordered_map<TargetEdgeId, std::size_t> edgeIndices_;
};

class TargetDFGBuilder {
public:
  explicit TargetDFGBuilder(std::string name = {}, std::string targetName = {})
      : dfg_(std::move(name), std::move(targetName)) {}

  TargetNodeId addNode(TargetNode node);
  TargetEdgeId addEdge(TargetEdge edge);
  void addExternal(ir::ExternalValue value);
  void addConstant(ir::ConstantValue value);
  void addLiveOut(TargetLiveOut liveOut);
  void addBinding(TargetOperandBinding binding);
  TargetDFG finish() { return std::move(dfg_); }

private:
  TargetDFG dfg_;
};

} // namespace cgra::target
