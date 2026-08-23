// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cgra::target {

enum class TargetDFGDiagnosticCode : std::uint32_t {
  TDFG_UNKNOWN_NODE,
  TDFG_DUPLICATE_NODE,
  TDFG_DUPLICATE_EDGE,
  TDFG_DUPLICATE_EXTERNAL,
  TDFG_DUPLICATE_CONSTANT,
  TDFG_DUPLICATE_LIVEOUT,
  TDFG_ADJACENCY_INCONSISTENT,
  TDFG_UNKNOWN_EDGE_SOURCE,
  TDFG_UNKNOWN_EDGE_DESTINATION,
  TDFG_UNKNOWN_OPERATION,
  TDFG_NO_COMPATIBLE_EXECUTION_RESOURCE,
  TDFG_EXECUTION_CLASS_MISMATCH,
  TDFG_ISSUE_OCCUPANCY_MISMATCH,
  TDFG_RESULT_LATENCY_MISMATCH,
  TDFG_PRODUCER_OUTPUT_READY_MISMATCH,
  TDFG_MEMORY_ACCESS_WIDTH_MISMATCH,
  TDFG_RESULT_TYPE_MISMATCH,
  TDFG_UNSUPPORTED_TYPE,
  TDFG_OPERAND_INDEX_OUT_OF_RANGE,
  TDFG_MISSING_PROVIDER,
  TDFG_DUPLICATE_PROVIDER,
  TDFG_DATA_EDGE_INVALID,
  TDFG_PREDICATE_EDGE_INVALID,
  TDFG_MEMORY_EDGE_INVALID,
  TDFG_STORE_RESULT_INVALID,
  TDFG_LOAD_RESULT_INVALID,
  TDFG_OPERATION_ARITY_INVALID,
  TDFG_OPERATION_OPERAND_INVALID,
  TDFG_BINDING_UNKNOWN_NODE,
  TDFG_BINDING_UNKNOWN_EXTERNAL,
  TDFG_BINDING_UNKNOWN_CONSTANT,
  TDFG_BINDING_TYPE_MISMATCH,
  TDFG_PROVENANCE_EMPTY,
  TDFG_PROVENANCE_UNKNOWN_GENERIC_NODE,
  TDFG_LIVEOUT_UNKNOWN_NODE,
  TDFG_LIVEOUT_TYPE_MISMATCH,
};

std::string_view toString(TargetDFGDiagnosticCode code);

struct TargetDFGDiagnostic {
  TargetDFGDiagnosticCode code = TargetDFGDiagnosticCode::TDFG_UNKNOWN_NODE;
  std::string message;
  std::optional<TargetNodeId> node;
  std::optional<TargetEdgeId> edge;
  std::optional<std::uint32_t> operand;

  TargetDFGDiagnostic() = default;
  TargetDFGDiagnostic(TargetDFGDiagnosticCode code, std::string message,
                      std::optional<TargetNodeId> node = std::nullopt,
                      std::optional<TargetEdgeId> edge = std::nullopt,
                      std::optional<std::uint32_t> operand = std::nullopt)
      : code(code), message(std::move(message)), node(node), edge(edge), operand(operand) {}
};

class TargetDFGVerificationReport {
public:
  bool ok() const noexcept { return diagnostics_.empty(); }
  std::span<const TargetDFGDiagnostic> diagnostics() const noexcept { return diagnostics_; }
  bool contains(TargetDFGDiagnosticCode code) const noexcept;
  std::string format() const;
  std::string toJson() const;

private:
  friend class TargetDFGVerifier;
  void add(TargetDFGDiagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }
  std::vector<TargetDFGDiagnostic> diagnostics_;
};

class TargetDFGVerifier {
public:
  static TargetDFGVerificationReport verify(const TargetDFG& dfg, const TargetModel& target,
                                            const ir::DFG* generic = nullptr);
};

} // namespace cgra::target
