// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetDFG.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::target {

TargetDFG::TargetDFG(std::string name, std::string targetName)
    : name_(std::move(name)), targetName_(std::move(targetName)) {}

bool TargetDFG::containsNode(TargetNodeId id) const noexcept { return nodeIndices_.contains(id); }
bool TargetDFG::containsEdge(TargetEdgeId id) const noexcept { return edgeIndices_.contains(id); }

const TargetNode& TargetDFG::node(TargetNodeId id) const {
  const auto it = nodeIndices_.find(id);
  if (it == nodeIndices_.end())
    throw std::out_of_range("unknown target DFG node id: " + std::to_string(id));
  return nodes_.at(it->second);
}

const TargetEdge& TargetDFG::edge(TargetEdgeId id) const {
  const auto it = edgeIndices_.find(id);
  if (it == edgeIndices_.end())
    throw std::out_of_range("unknown target DFG edge id: " + std::to_string(id));
  return edges_.at(it->second);
}

std::span<const TargetEdgeId> TargetDFG::incoming(TargetNodeId id) const {
  const auto nodeIt = nodeIndices_.find(id);
  if (nodeIt == nodeIndices_.end())
    throw std::out_of_range("unknown target DFG node id: " + std::to_string(id));
  return incoming_.at(nodeIt->second);
}

std::span<const TargetEdgeId> TargetDFG::outgoing(TargetNodeId id) const {
  const auto nodeIt = nodeIndices_.find(id);
  if (nodeIt == nodeIndices_.end())
    throw std::out_of_range("unknown target DFG node id: " + std::to_string(id));
  return outgoing_.at(nodeIt->second);
}

TargetNodeId TargetDFG::appendNode(TargetNode node) {
  if (nodeIndices_.contains(node.id))
    throw std::invalid_argument("duplicate target DFG node ID");
  nodeIndices_.emplace(node.id, nodes_.size());
  nodes_.push_back(std::move(node));
  incoming_.emplace_back();
  outgoing_.emplace_back();
  return nodes_.back().id;
}

TargetEdgeId TargetDFG::appendEdge(TargetEdge edge) {
  if (edgeIndices_.contains(edge.id))
    throw std::invalid_argument("duplicate target DFG edge ID");
  if (!containsNode(edge.src) || !containsNode(edge.dst))
    throw std::invalid_argument("target DFG edge endpoints must reference existing nodes");
  edgeIndices_.emplace(edge.id, edges_.size());
  edges_.push_back(std::move(edge));
  incoming_.at(nodeIndices_.at(edges_.back().dst)).push_back(edges_.back().id);
  outgoing_.at(nodeIndices_.at(edges_.back().src)).push_back(edges_.back().id);
  return edges_.back().id;
}

void TargetDFG::appendExternal(ir::ExternalValue value) {
  externalValues_.push_back(std::move(value));
}
void TargetDFG::appendConstant(ir::ConstantValue value) { constants_.push_back(std::move(value)); }
void TargetDFG::appendLiveOut(TargetLiveOut liveOut) { liveOuts_.push_back(std::move(liveOut)); }

void TargetDFG::appendBinding(TargetOperandBinding binding) {
  bindings_.push_back(std::move(binding));
  std::sort(bindings_.begin(), bindings_.end(), [](const auto& lhs, const auto& rhs) {
    return std::pair{lhs.node, lhs.operand} < std::pair{rhs.node, rhs.operand};
  });
}

bool TargetDFG::operator==(const TargetDFG& other) const noexcept {
  return name_ == other.name_ && targetName_ == other.targetName_ && nodes_ == other.nodes_ &&
         edges_ == other.edges_ && externalValues_ == other.externalValues_ &&
         constants_ == other.constants_ && liveOuts_ == other.liveOuts_ &&
         bindings_ == other.bindings_;
}

TargetNodeId TargetDFGBuilder::addNode(TargetNode node) { return dfg_.appendNode(std::move(node)); }
TargetEdgeId TargetDFGBuilder::addEdge(TargetEdge edge) { return dfg_.appendEdge(std::move(edge)); }
void TargetDFGBuilder::addExternal(ir::ExternalValue value) {
  dfg_.appendExternal(std::move(value));
}
void TargetDFGBuilder::addConstant(ir::ConstantValue value) {
  dfg_.appendConstant(std::move(value));
}
void TargetDFGBuilder::addLiveOut(TargetLiveOut liveOut) { dfg_.appendLiveOut(std::move(liveOut)); }
void TargetDFGBuilder::addBinding(TargetOperandBinding binding) {
  dfg_.appendBinding(std::move(binding));
}

} // namespace cgra::target
