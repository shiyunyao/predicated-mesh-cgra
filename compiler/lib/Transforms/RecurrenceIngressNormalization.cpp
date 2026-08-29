// SPDX-License-Identifier: MIT
#include "cgra/Transforms/RecurrenceIngressNormalization.h"

#include "cgra/IR/DFGBuilder.h"

#include <algorithm>
#include <sstream>
#include <type_traits>
#include <variant>

namespace cgra::transforms {
namespace {

bool isEligible(const ir::Edge& edge, const RecurrenceIngressOptions& options) {
  if (edge.distance == 0 || edge.distance > options.maxDistance ||
      edge.kind() == ir::Edge::Kind::Memory)
    return false;
  return edge.kind() == ir::Edge::Kind::Data ? options.enableData : options.enablePredicate;
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

std::string boundaryHash(const std::optional<ir::RecurrenceBoundary>& boundary) {
  if (!boundary)
    return "none";
  // This is a compact deterministic provenance key, not a cryptographic hash.
  std::ostringstream output;
  for (const auto& value : boundary->values) {
    output << value.iterationOffset << ':';
    std::visit([&](const auto& source) {
      using Source = std::decay_t<decltype(source)>;
      if constexpr (std::is_same_v<Source, ir::ExternalValueRef>)
        output << "e" << source.value;
      else
        output << "c" << source.value;
    }, value.value);
    output << ';';
  }
  return output.str();
}

std::optional<std::uint32_t> destinationOperand(const ir::Edge& edge) {
  if (edge.kind() == ir::Edge::Kind::Data)
    return std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
  if (edge.kind() == ir::Edge::Kind::Predicate)
    return std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
  return std::nullopt;
}

std::optional<ir::RecurrenceBoundary> recurrenceBoundary(const ir::Edge& edge) {
  if (edge.kind() == ir::Edge::Kind::Data)
    return std::get<ir::DataEdgeInfo>(edge.info).boundary;
  if (edge.kind() == ir::Edge::Kind::Predicate)
    return std::get<ir::PredicateEdgeInfo>(edge.info).boundary;
  return std::nullopt;
}

struct RecurrenceGroup {
  const ir::Edge* representative = nullptr;
  std::vector<const ir::Edge*> consumers;
};

} // namespace

RecurrenceIngressNormalizationResult normalizeRecurrenceIngress(
    ir::DFG dfg, const RecurrenceIngressOptions& options) {
  RecurrenceIngressNormalizationResult result;
  ir::DFGBuilder builder(dfg.name());
  for (const auto& value : dfg.externalValues())
    builder.importExternal(value);
  for (const auto& value : dfg.constants())
    builder.importConstant(value);
  for (const auto& node : dfg.nodes())
    builder.importNode(node);

  std::vector<const ir::Edge*> recurrenceEdges;
  for (const auto& edge : dfg.edges()) {
    if (isEligible(edge, options))
      recurrenceEdges.push_back(&edge);
    else if (edge.distance > options.maxDistance && edge.kind() != ir::Edge::Kind::Memory)
      result.diagnostics.push_back("distance " + std::to_string(edge.distance) +
                                   " recurrence retained (maxDistance=" +
                                   std::to_string(options.maxDistance) + ")");
  }

  // A shared ingress is safe only when all source, type, distance, boundary,
  // and network kind facts agree. Destination operand is intentionally not a
  // grouping key: one ingress fans out to each original consumer.
  std::vector<RecurrenceGroup> groups;
  for (const auto* edge : recurrenceEdges) {
    const auto& source = dfg.node(edge->src);
    const auto type = source.resultType;
    const auto boundary = recurrenceBoundary(*edge);
    auto group = std::find_if(groups.begin(), groups.end(), [&](const RecurrenceGroup& candidate) {
      const auto& representative = *candidate.representative;
      const auto& repSource = dfg.node(representative.src);
      return representative.src == edge->src && representative.kind() == edge->kind() &&
             representative.distance == edge->distance && repSource.resultType == type &&
             recurrenceBoundary(representative) == boundary;
    });
    if (group == groups.end())
      groups.push_back({edge, {edge}});
    else
      group->consumers.push_back(edge);
  }

  std::vector<ir::NodeId> ingressNodes;
  ingressNodes.reserve(groups.size());
  for (const auto& group : groups) {
    const auto& edge = *group.representative;
    const auto kind = edge.kind() == ir::Edge::Kind::Predicate ? RecurrenceIngressKind::Predicate
                                                                 : RecurrenceIngressKind::Data;
    ingressNodes.push_back(addIngress(builder, dfg.node(edge.dst),
                                       destinationOperand(edge).value(), kind));
  }

  ir::EdgeId nextGeneratedEdge = 0;
  for (const auto& edge : dfg.edges())
    nextGeneratedEdge = std::max(nextGeneratedEdge, edge.id + 1);

  for (const auto& binding : dfg.externalBindings())
    importBinding(builder, binding);

  // Import all unchanged edges first so generated local edges receive stable IDs
  // above the source graph's ID range.
  for (const auto& edge : dfg.edges()) {
    if (isEligible(edge, options))
      continue;
    builder.importEdge(edge);
  }

  for (std::size_t index = 0; index < groups.size(); ++index) {
    const auto& group = groups[index];
    const auto& edge = *group.representative;
    const auto& source = dfg.node(edge.src);
    const auto ingress = ingressNodes[index];
    auto recurrence = edge;
    recurrence.dst = ingress;
    recurrence.info = edge.kind() == ir::Edge::Kind::Predicate
                          ? ir::EdgeInfo{ir::PredicateEdgeInfo{0,
                                                               std::get<ir::PredicateEdgeInfo>(edge.info).boundary}}
                          : ir::EdgeInfo{ir::DataEdgeInfo{0,
                                                          std::get<ir::DataEdgeInfo>(edge.info).boundary}};
    builder.importEdge(recurrence);
    // Reserve generated IDs above the complete source range. Importing source
    // recurrence edges after local edges must never collide with a generated
    // ID when the source graph has sparse edge numbering.
    for (const auto* consumer : group.consumers) {
      const auto local = nextGeneratedEdge++;
      const auto operand = destinationOperand(*consumer).value();
      builder.importEdge(consumer->kind() == ir::Edge::Kind::Predicate
                             ? ir::Edge{local, ingress, consumer->dst, 0,
                                        ir::PredicateEdgeInfo{operand, std::nullopt}}
                             : ir::Edge{local, ingress, consumer->dst, 0,
                                        ir::DataEdgeInfo{operand, std::nullopt}});
      const auto& destination = dfg.node(consumer->dst);
      result.records.push_back({consumer->id,
                                consumer->src,
                                consumer->dst,
                                operand,
                                ingress,
                                recurrence.id,
                                local,
                                consumer->kind() == ir::Edge::Kind::Predicate
                                    ? RecurrenceIngressKind::Predicate
                                    : RecurrenceIngressKind::Data,
                                destination.operandTypes.at(operand),
                                consumer->distance,
                                boundaryHash(recurrenceBoundary(*consumer)),
                                source.source ? source.source->label : "",
                                static_cast<std::uint32_t>(group.consumers.size()),
                                group.consumers.size() > 1});
    }
  }

  for (const auto& liveOut : dfg.liveOuts())
    builder.importLiveOut(liveOut);
  result.dfg = builder.finish();
  return result;
}

} // namespace cgra::transforms
