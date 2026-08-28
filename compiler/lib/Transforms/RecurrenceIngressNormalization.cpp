// SPDX-License-Identifier: MIT
#include "cgra/Transforms/RecurrenceIngressNormalization.h"

#include "cgra/IR/DFGBuilder.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace cgra::transforms {
namespace {

bool isRecurrence(const ir::Edge& edge) {
  return edge.distance != 0 && edge.kind() != ir::Edge::Kind::Memory;
}

ir::NodeId addIngress(ir::DFGBuilder& builder, const ir::Node& destination,
                      std::uint32_t destinationOperand, RecurrenceIngressKind kind) {
  const auto key = kind == RecurrenceIngressKind::Predicate ? "PPASS" : "PASS";
  const auto type = destination.operandTypes.at(destinationOperand);
  return builder.addCustomNode(key, {type}, type, ir::SourceInfo{"recurrence_ingress"});
}

void importBinding(ir::DFGBuilder& builder, const ir::OperandBinding& binding) {
  std::visit(
      [&](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, ir::ExternalValueRef>)
          builder.bindExternal(binding.node, binding.operand, source.value);
        else
          builder.bindConstant(binding.node, binding.operand, source.value);
      },
      binding.source);
}

} // namespace

RecurrenceIngressNormalizationResult normalizeRecurrenceIngress(ir::DFG dfg) {
  RecurrenceIngressNormalizationResult result;
  ir::DFGBuilder builder(dfg.name());
  for (const auto& value : dfg.externalValues())
    builder.importExternal(value);
  for (const auto& value : dfg.constants())
    builder.importConstant(value);
  for (const auto& node : dfg.nodes())
    builder.importNode(node);

  std::vector<const ir::Edge*> recurrenceEdges;
  for (const auto& edge : dfg.edges())
    if (isRecurrence(edge))
      recurrenceEdges.push_back(&edge);

  std::vector<ir::NodeId> ingressNodes;
  ingressNodes.reserve(recurrenceEdges.size());
  for (const auto* edge : recurrenceEdges) {
    const auto& destination = dfg.node(edge->dst);
    const auto kind = edge->kind() == ir::Edge::Kind::Predicate ? RecurrenceIngressKind::Predicate
                                                                 : RecurrenceIngressKind::Data;
    const auto destinationOperand = edge->kind() == ir::Edge::Kind::Predicate
                                        ? std::get<ir::PredicateEdgeInfo>(edge->info).dstOperand
                                        : std::get<ir::DataEdgeInfo>(edge->info).dstOperand;
    ingressNodes.push_back(addIngress(builder, destination, destinationOperand, kind));
  }

  ir::EdgeId nextGeneratedEdge = 0;
  for (const auto& edge : dfg.edges())
    nextGeneratedEdge = std::max(nextGeneratedEdge, edge.id + 1);

  for (const auto& binding : dfg.externalBindings())
    importBinding(builder, binding);

  // Import all unchanged edges first so generated local edges receive stable IDs
  // above the source graph's ID range.
  for (const auto& edge : dfg.edges()) {
    if (isRecurrence(edge))
      continue;
    builder.importEdge(edge);
  }

  for (std::size_t index = 0; index < recurrenceEdges.size(); ++index) {
    const auto& edge = *recurrenceEdges[index];
    const auto ingress = ingressNodes[index];
    auto recurrence = edge;
    recurrence.dst = ingress;
    recurrence.info = edge.kind() == ir::Edge::Kind::Predicate
                          ? ir::EdgeInfo{ir::PredicateEdgeInfo{0,
                                                               std::get<ir::PredicateEdgeInfo>(edge.info).boundary}}
                          : ir::EdgeInfo{ir::DataEdgeInfo{0,
                                                          std::get<ir::DataEdgeInfo>(edge.info).boundary}};
    builder.importEdge(recurrence);
    const auto destinationOperand = edge.kind() == ir::Edge::Kind::Predicate
                                        ? std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand
                                        : std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
    // Reserve generated IDs above the complete source range. Importing source
    // recurrence edges after local edges must never collide with a generated
    // ID when the source graph has sparse edge numbering.
    const auto local = nextGeneratedEdge++;
    builder.importEdge(edge.kind() == ir::Edge::Kind::Predicate
                           ? ir::Edge{local, ingress, edge.dst, 0,
                                      ir::PredicateEdgeInfo{destinationOperand, std::nullopt}}
                           : ir::Edge{local, ingress, edge.dst, 0,
                                      ir::DataEdgeInfo{destinationOperand, std::nullopt}});
    result.records.push_back({edge.id, ingress, recurrence.id, local,
                              edge.kind() == ir::Edge::Kind::Predicate
                                  ? RecurrenceIngressKind::Predicate
                                  : RecurrenceIngressKind::Data,
                              edge.distance});
  }

  for (const auto& liveOut : dfg.liveOuts())
    builder.importLiveOut(liveOut);
  result.dfg = builder.finish();
  return result;
}

} // namespace cgra::transforms
