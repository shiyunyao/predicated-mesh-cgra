// SPDX-License-Identifier: MIT
#include "cgra/IR/DFG.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::ir {

DFG::DFG(std::string name) : name_(std::move(name)) {}

bool DFG::containsNode(NodeId id) const noexcept { return nodeIndices_.contains(id); }
bool DFG::containsEdge(EdgeId id) const noexcept { return edgeIndices_.contains(id); }
bool DFG::containsExternal(ExternalValueId id) const noexcept {
  return externalIndices_.contains(id);
}
bool DFG::containsConstant(ConstantId id) const noexcept { return constantIndices_.contains(id); }
bool DFG::containsLiveOut(LiveOutId id) const noexcept { return liveOutIndices_.contains(id); }

const Node& DFG::node(NodeId id) const {
  const auto it = nodeIndices_.find(id);
  if (it == nodeIndices_.end())
    throw std::out_of_range("unknown DFG node id: " + std::to_string(id));
  return nodes_.at(it->second);
}

const Edge& DFG::edge(EdgeId id) const {
  const auto it = edgeIndices_.find(id);
  if (it == edgeIndices_.end())
    throw std::out_of_range("unknown DFG edge id: " + std::to_string(id));
  return edges_.at(it->second);
}

const ExternalValue& DFG::externalValue(ExternalValueId id) const {
  const auto it = externalIndices_.find(id);
  if (it == externalIndices_.end())
    throw std::out_of_range("unknown DFG external value id: " + std::to_string(id));
  return externalValues_.at(it->second);
}

const ConstantValue& DFG::constant(ConstantId id) const {
  const auto it = constantIndices_.find(id);
  if (it == constantIndices_.end())
    throw std::out_of_range("unknown DFG constant id: " + std::to_string(id));
  return constants_.at(it->second);
}

const LiveOut& DFG::liveOut(LiveOutId id) const {
  const auto it = liveOutIndices_.find(id);
  if (it == liveOutIndices_.end())
    throw std::out_of_range("unknown DFG live-out id: " + std::to_string(id));
  return liveOuts_.at(it->second);
}

std::span<const EdgeId> DFG::incoming(NodeId id) const {
  return incoming_.at(nodeIndices_.at(node(id).id));
}

std::span<const EdgeId> DFG::outgoing(NodeId id) const {
  return outgoing_.at(nodeIndices_.at(node(id).id));
}

NodeId DFG::appendNode(Node node) {
  if (node.id != nodes_.size() || nodeIndices_.contains(node.id))
    throw std::invalid_argument("DFG node IDs must be unique and allocated in order");
  nodeIndices_.emplace(node.id, nodes_.size());
  nodes_.push_back(std::move(node));
  incoming_.emplace_back();
  outgoing_.emplace_back();
  return nodes_.back().id;
}

ExternalValueId DFG::appendExternal(ExternalValue value) {
  if (value.id != externalValues_.size() || externalIndices_.contains(value.id))
    throw std::invalid_argument("DFG external value IDs must be allocated in order");
  externalIndices_.emplace(value.id, externalValues_.size());
  externalValues_.push_back(std::move(value));
  return externalValues_.back().id;
}

ConstantId DFG::appendConstant(ConstantValue value) {
  if (value.id != constants_.size() || constantIndices_.contains(value.id))
    throw std::invalid_argument("DFG constant IDs must be allocated in order");
  constantIndices_.emplace(value.id, constants_.size());
  constants_.push_back(std::move(value));
  return constants_.back().id;
}

LiveOutId DFG::appendLiveOut(LiveOut value) {
  if (value.id != liveOuts_.size() || liveOutIndices_.contains(value.id))
    throw std::invalid_argument("DFG live-out IDs must be allocated in order");
  if (!containsNode(value.source))
    throw std::invalid_argument("DFG live-out references an unknown source node");
  liveOutIndices_.emplace(value.id, liveOuts_.size());
  liveOuts_.push_back(std::move(value));
  return liveOuts_.back().id;
}

EdgeId DFG::appendEdge(Edge edge) {
  if (edge.id != edges_.size() || edgeIndices_.contains(edge.id))
    throw std::invalid_argument("DFG edge IDs must be unique and allocated in order");
  if (!containsNode(edge.src) || !containsNode(edge.dst))
    throw std::invalid_argument("DFG edge endpoints must reference existing nodes");
  edgeIndices_.emplace(edge.id, edges_.size());
  edges_.push_back(std::move(edge));
  incoming_.at(nodeIndices_.at(edges_.back().dst)).push_back(edges_.back().id);
  outgoing_.at(nodeIndices_.at(edges_.back().src)).push_back(edges_.back().id);
  return edges_.back().id;
}

void DFG::appendBinding(OperandBinding binding) {
  if (!containsNode(binding.node))
    throw std::invalid_argument("DFG operand binding references unknown node");
  const auto duplicate = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
    return item.node == binding.node && item.operand == binding.operand;
  });
  if (duplicate != bindings_.end())
    throw std::invalid_argument("DFG operand binding is duplicated");
  bindings_.push_back(std::move(binding));
  std::sort(bindings_.begin(), bindings_.end(), [](const auto& lhs, const auto& rhs) {
    return std::pair{lhs.node, lhs.operand} < std::pair{rhs.node, rhs.operand};
  });
}

bool DFG::operator==(const DFG& other) const noexcept {
  return name_ == other.name_ && nodes_ == other.nodes_ && edges_ == other.edges_ &&
         externalValues_ == other.externalValues_ && constants_ == other.constants_ &&
         liveOuts_ == other.liveOuts_ && bindings_ == other.bindings_;
}

} // namespace cgra::ir
