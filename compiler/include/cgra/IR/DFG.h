// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/Edge.h"

#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgra::ir {

class DFGBuilder;

class DFG {
public:
  explicit DFG(std::string name = {});

  const std::string& name() const noexcept { return name_; }
  std::span<const Node> nodes() const noexcept { return nodes_; }
  std::span<const Edge> edges() const noexcept { return edges_; }
  std::span<const ExternalValue> externalValues() const noexcept { return externalValues_; }
  std::span<const ConstantValue> constants() const noexcept { return constants_; }
  std::span<const LiveOut> liveOuts() const noexcept { return liveOuts_; }
  std::span<const OperandBinding> externalBindings() const noexcept { return bindings_; }

  bool containsNode(NodeId id) const noexcept;
  bool containsEdge(EdgeId id) const noexcept;
  bool containsExternal(ExternalValueId id) const noexcept;
  bool containsConstant(ConstantId id) const noexcept;
  bool containsLiveOut(LiveOutId id) const noexcept;

  const Node& node(NodeId id) const;
  const Edge& edge(EdgeId id) const;
  const ExternalValue& externalValue(ExternalValueId id) const;
  const ConstantValue& constant(ConstantId id) const;
  const LiveOut& liveOut(LiveOutId id) const;
  std::span<const EdgeId> incoming(NodeId id) const;
  std::span<const EdgeId> outgoing(NodeId id) const;

  bool operator==(const DFG& other) const noexcept;

private:
  friend class DFGBuilder;

  NodeId appendNode(Node node);
  ExternalValueId appendExternal(ExternalValue value);
  ConstantId appendConstant(ConstantValue value);
  LiveOutId appendLiveOut(LiveOut value);
  EdgeId appendEdge(Edge edge);
  void appendBinding(OperandBinding binding);

  std::string name_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<ExternalValue> externalValues_;
  std::vector<ConstantValue> constants_;
  std::vector<LiveOut> liveOuts_;
  std::vector<OperandBinding> bindings_;
  std::vector<std::vector<EdgeId>> incoming_;
  std::vector<std::vector<EdgeId>> outgoing_;
  std::unordered_map<NodeId, std::size_t> nodeIndices_;
  std::unordered_map<EdgeId, std::size_t> edgeIndices_;
  std::unordered_map<ExternalValueId, std::size_t> externalIndices_;
  std::unordered_map<ConstantId, std::size_t> constantIndices_;
  std::unordered_map<LiveOutId, std::size_t> liveOutIndices_;
};

} // namespace cgra::ir
