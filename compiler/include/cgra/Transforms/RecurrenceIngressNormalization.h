// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cgra::transforms {

enum class RecurrenceIngressKind {
  Data,
  Predicate,
};

struct RecurrenceIngressOptions {
  bool enableData = true;
  bool enablePredicate = true;
  std::uint32_t maxDistance = 1;
};

struct RecurrenceIngressRecord {
  ir::EdgeId originalEdge = 0;
  ir::NodeId originalSource = 0;
  ir::NodeId originalDestination = 0;
  std::uint32_t originalDestinationOperand = 0;
  ir::NodeId ingressNode = 0;
  ir::EdgeId recurrenceEdge = 0;
  ir::EdgeId localEdge = 0;
  RecurrenceIngressKind kind = RecurrenceIngressKind::Data;
  ir::ValueType valueType = ir::ValueType::voidTy();
  std::uint32_t distance = 1;
  std::string boundaryHash;
  std::string sourceRecurrenceProvenance;
  std::uint32_t consumerCount = 1;
  bool sharedIngress = false;
};

struct RecurrenceIngressNormalizationResult {
  ir::DFG dfg;
  std::vector<RecurrenceIngressRecord> records;
  std::vector<std::string> diagnostics;

  bool changed() const noexcept { return !records.empty(); }
};

// Normalize loop-carried value edges without touching memory-dependence edges.
// The returned DFG is always independently verifiable by DFGVerifier.
RecurrenceIngressNormalizationResult
normalizeRecurrenceIngress(ir::DFG dfg, const RecurrenceIngressOptions& options = {});

} // namespace cgra::transforms
