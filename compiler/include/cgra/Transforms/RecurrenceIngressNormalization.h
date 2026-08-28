// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cgra::transforms {

enum class RecurrenceIngressKind {
  Data,
  Predicate,
};

struct RecurrenceIngressRecord {
  ir::EdgeId originalEdge = 0;
  ir::NodeId ingressNode = 0;
  ir::EdgeId recurrenceEdge = 0;
  ir::EdgeId localEdge = 0;
  RecurrenceIngressKind kind = RecurrenceIngressKind::Data;
  std::uint32_t distance = 1;
};

struct RecurrenceIngressNormalizationResult {
  ir::DFG dfg;
  std::vector<RecurrenceIngressRecord> records;
  std::vector<std::string> diagnostics;

  bool changed() const noexcept { return !records.empty(); }
};

// Normalize loop-carried value edges without touching memory-dependence edges.
// The returned DFG is always independently verifiable by DFGVerifier.
RecurrenceIngressNormalizationResult normalizeRecurrenceIngress(ir::DFG dfg);

} // namespace cgra::transforms
