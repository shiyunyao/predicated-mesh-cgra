// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::ir {

enum class DiagnosticSeverity {
  Error,
  Warning,
};

enum class DFGDiagnosticCode : std::uint32_t {
  DFG_ID_DUPLICATE_NODE,
  DFG_ID_DUPLICATE_EDGE,
  DFG_ID_DUPLICATE_EXTERNAL,
  DFG_ID_DUPLICATE_CONSTANT,
  DFG_ID_DUPLICATE_LIVEOUT,
  DFG_EDGE_UNKNOWN_SOURCE,
  DFG_EDGE_UNKNOWN_DESTINATION,
  DFG_BINDING_UNKNOWN_NODE,
  DFG_BINDING_UNKNOWN_EXTERNAL,
  DFG_BINDING_UNKNOWN_CONSTANT,
  DFG_ADJACENCY_INCONSISTENT,

  DFG_OPERAND_INDEX_OUT_OF_RANGE,
  DFG_OPERAND_MISSING_PROVIDER,
  DFG_OPERAND_DUPLICATE_PROVIDER,
  DFG_OPERAND_TYPE_MISMATCH,
  DFG_OPERAND_INVALID_EDGE_KIND,

  DFG_TYPE_INVALID,
  DFG_RESULT_TYPE_INVALID,
  DFG_EDGE_VALUE_TYPE_MISMATCH,
  DFG_PREDICATE_EXPECTED,
  DFG_DATA_VALUE_EXPECTED,
  DFG_VOID_VALUE_USED,

  DFG_OPCODE_ARITY_MISMATCH,
  DFG_OPCODE_RESULT_TYPE_MISMATCH,
  DFG_OPCODE_OPERAND_TYPE_MISMATCH,
  DFG_OPCODE_UNEXPECTED_METADATA,
  DFG_OPCODE_MISSING_METADATA,
  DFG_OPCODE_METADATA_INVALID,

  DFG_ICMP_MISSING_PREDICATE,
  DFG_ICMP_INVALID_RESULT_TYPE,
  DFG_ICMP_OPERAND_TYPE_MISMATCH,
  DFG_SELECT_INVALID_PREDICATE,
  DFG_SELECT_VALUE_TYPE_MISMATCH,
  DFG_SELECT_RESULT_TYPE_MISMATCH,
  DFG_LOAD_INVALID_RESULT_TYPE,
  DFG_LOAD_INVALID_ADDRESS_TYPE,
  DFG_STORE_INVALID_RESULT_TYPE,
  DFG_STORE_INVALID_ADDRESS_TYPE,
  DFG_STORE_INVALID_VALUE_TYPE,
  DFG_STORE_INVALID_PREDICATE,

  DFG_DATA_EDGE_INVALID_SOURCE_TYPE,
  DFG_DATA_EDGE_INVALID_DEST_OPERAND,
  DFG_PRED_EDGE_INVALID_SOURCE_TYPE,
  DFG_PRED_EDGE_INVALID_DEST_OPERAND,
  DFG_MEMORY_EDGE_INVALID_SOURCE,
  DFG_MEMORY_EDGE_INVALID_DESTINATION,
  DFG_MEMORY_EDGE_INVALID_DEPENDENCE,
  DFG_MEMORY_EDGE_UNEXPECTED_OPERAND,
  DFG_RECURRENCE_BOUNDARY_MISSING,
  DFG_RECURRENCE_BOUNDARY_OFFSET_OUT_OF_RANGE,
  DFG_RECURRENCE_BOUNDARY_DUPLICATE_OFFSET,
  DFG_RECURRENCE_BOUNDARY_SOURCE_UNKNOWN,
  DFG_RECURRENCE_BOUNDARY_TYPE_MISMATCH,

  DFG_LIVEOUT_UNKNOWN_SOURCE,
  DFG_LIVEOUT_TYPE_MISMATCH,
  DFG_LIVEOUT_VOID_SOURCE,
};

std::string_view toString(DiagnosticSeverity severity);
std::string_view toString(DFGDiagnosticCode code);

struct DFGDiagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  DFGDiagnosticCode code = DFGDiagnosticCode::DFG_TYPE_INVALID;
  std::string message;
  std::optional<NodeId> node;
  std::optional<EdgeId> edge;
  std::optional<std::uint32_t> operand;
  std::optional<ExternalValueId> external;
  std::optional<ConstantId> constant;
  std::optional<LiveOutId> liveOut;
};

std::string formatDiagnostic(const DFGDiagnostic& diagnostic);

class DFGVerificationReport {
public:
  bool ok() const noexcept { return errorCount_ == 0; }
  std::size_t errorCount() const noexcept { return errorCount_; }
  std::size_t warningCount() const noexcept { return warningCount_; }
  bool contains(DFGDiagnosticCode code) const noexcept;
  std::span<const DFGDiagnostic> diagnostics() const noexcept { return diagnostics_; }

  std::string format() const;
  std::string toJson() const;
  void writeJson(const std::filesystem::path& path) const;

private:
  friend class DFGVerifier;
  friend class DFGVerifierImpl;

  void add(DFGDiagnostic diagnostic);

  std::vector<DFGDiagnostic> diagnostics_;
  std::size_t errorCount_ = 0;
  std::size_t warningCount_ = 0;
};

class DFGVerifier {
public:
  static DFGVerificationReport verify(const DFG& dfg);
};

} // namespace cgra::ir
