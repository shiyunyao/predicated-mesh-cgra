// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/StagedMapping.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cgra::schedule {

enum class StageAssignmentDiagnosticCode : std::uint32_t {
  STAGE_INVALID_TARGET_DFG,
  STAGE_INVALID_MODULO_MAPPING,
  STAGE_UNKNOWN_NODE,
  STAGE_MISSING_STAGE,
  STAGE_DUPLICATE_STAGE,
  STAGE_OUTPUT_ARITHMETIC_OVERFLOW,
  STAGE_CONSTRAINT_VIOLATION,
};

struct StageAssignmentDiagnostic {
  StageAssignmentDiagnosticCode code = StageAssignmentDiagnosticCode::STAGE_CONSTRAINT_VIOLATION;
  std::string message;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
};

class StageAssignmentVerificationReport {
public:
  bool ok() const noexcept { return diagnostics_.empty(); }
  bool contains(StageAssignmentDiagnosticCode code) const noexcept;
  std::span<const StageAssignmentDiagnostic> diagnostics() const noexcept { return diagnostics_; }
  std::string format() const;
  void add(StageAssignmentDiagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

private:
  friend class StageAssignmentVerifier;
  std::vector<StageAssignmentDiagnostic> diagnostics_;
};

class StageAssignmentVerifier {
public:
  static StageAssignmentVerificationReport verify(const cgra::target::TargetDFG& dfg,
                                                  const cgra::TargetModel& target,
                                                  const StagedMapping& mapping);
};

} // namespace cgra::schedule
