// SPDX-License-Identifier: MIT
#include "cgra/Transforms/RecurrenceIngressVerifier.h"

#include "cgra/IR/DFGVerifier.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace cgra::transforms {
namespace {

using EdgeKind = ir::Edge::Kind;

std::optional<std::uint32_t> operand(const ir::Edge& edge) {
  if (edge.kind() == EdgeKind::Data)
    return std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
  if (edge.kind() == EdgeKind::Predicate)
    return std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
  return std::nullopt;
}

std::optional<ir::RecurrenceBoundary> boundary(const ir::Edge& edge) {
  if (edge.kind() == EdgeKind::Data)
    return std::get<ir::DataEdgeInfo>(edge.info).boundary;
  if (edge.kind() == EdgeKind::Predicate)
    return std::get<ir::PredicateEdgeInfo>(edge.info).boundary;
  return std::nullopt;
}

void fail(RecurrenceIngressVerificationResult& result, std::string message) {
  result.diagnostics.push_back(std::move(message));
}

} // namespace

std::string RecurrenceIngressVerificationResult::format() const {
  std::ostringstream output;
  output << (ok ? "recurrence ingress verification passed" :
                    "recurrence ingress verification failed");
  for (const auto& diagnostic : diagnostics)
    output << "\n- " << diagnostic;
  return output.str();
}

RecurrenceIngressVerificationResult verifyRecurrenceIngress(
    const ir::DFG& original, const RecurrenceIngressNormalizationResult& normalized) {
  RecurrenceIngressVerificationResult result;
  const auto generic = ir::DFGVerifier::verify(normalized.dfg);
  if (!generic.ok())
    fail(result, "normalized DFG verifier failed: " + generic.format());

  std::unordered_set<ir::EdgeId> seenOriginal;
  std::unordered_set<ir::EdgeId> seenRecurrence;
  std::unordered_set<ir::EdgeId> seenLocal;
  for (const auto& record : normalized.records) {
    if (!seenOriginal.insert(record.originalEdge).second)
      fail(result, "duplicate original recurrence edge record " +
                       std::to_string(record.originalEdge));
    if (!original.containsEdge(record.originalEdge)) {
      fail(result, "original recurrence edge is missing " +
                       std::to_string(record.originalEdge));
      continue;
    }
    const auto& source = original.edge(record.originalEdge);
    if (source.kind() == EdgeKind::Memory || source.distance == 0) {
      fail(result, "record does not identify a loop-carried data/predicate edge " +
                       std::to_string(record.originalEdge));
      continue;
    }
    if (!normalized.dfg.containsEdge(record.recurrenceEdge) ||
        !normalized.dfg.containsEdge(record.localEdge)) {
      fail(result, "replacement edge missing for original edge " +
                       std::to_string(record.originalEdge));
      continue;
    }
    const auto& recurrence = normalized.dfg.edge(record.recurrenceEdge);
    const auto& local = normalized.dfg.edge(record.localEdge);
    if (recurrence.src != source.src || recurrence.dst != record.ingressNode ||
        recurrence.distance != source.distance || recurrence.kind() != source.kind() ||
        boundary(recurrence) != boundary(source))
      fail(result, "recurrence replacement changed source, distance, kind, or boundary for " +
                       std::to_string(record.originalEdge));
    if (local.src != record.ingressNode || local.dst != source.dst || local.distance != 0 ||
        local.kind() != source.kind() || operand(local) != operand(source) || boundary(local))
      fail(result, "local replacement changed destination operand or carries a boundary for " +
                       std::to_string(record.originalEdge));
    // The recurrence replacement intentionally retains the original edge ID so
    // downstream provenance remains stable. It is valid for originalEdge and
    // recurrenceEdge to be equal; the endpoint/distance checks above prove it
    // is the replacement rather than an untouched edge.
    seenRecurrence.insert(record.recurrenceEdge);
    if (!seenLocal.insert(record.localEdge).second)
      fail(result, "duplicate local replacement edge " + std::to_string(record.localEdge));
  }

  // Every edge outside the recorded recurrence replacements must survive
  // unchanged. This catches dropped data/predicate edges as well as accidental
  // rewrites of artificial ordering edges.
  for (const auto& edge : original.edges()) {
    if (seenOriginal.count(edge.id) != 0)
      continue;
    if (!normalized.dfg.containsEdge(edge.id)) {
      fail(result, "unrecorded original edge was dropped " + std::to_string(edge.id));
    } else if (normalized.dfg.edge(edge.id) != edge) {
      fail(result, "unrecorded original edge was changed " + std::to_string(edge.id));
    }
  }

  for (const auto& edge : original.edges()) {
    if (edge.kind() == EdgeKind::Memory) {
      if (!normalized.dfg.containsEdge(edge.id)) {
        fail(result, "memory edge was changed by recurrence ingress normalization " +
                         std::to_string(edge.id));
      } else if (normalized.dfg.edge(edge.id) != edge) {
        fail(result, "memory edge payload was changed by recurrence ingress normalization " +
                         std::to_string(edge.id));
      }
    }
  }
  result.ok = result.diagnostics.empty();
  return result;
}

} // namespace cgra::transforms
