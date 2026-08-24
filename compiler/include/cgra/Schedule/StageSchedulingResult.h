// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/StageConstraint.h"
#include "cgra/Schedule/StagedMapping.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::schedule {

enum class StageSchedulingStatus {
  Success,
  InvalidTargetDFG,
  InvalidModuloMapping,
  InfeasibleStageConstraints,
  ArithmeticOverflow,
  VerificationFailure,
  InternalError,
};

enum class StageSchedulingDiagnosticCode : std::uint32_t {
  STAGE_INVALID_TARGET_DFG,
  STAGE_INVALID_MODULO_MAPPING,
  STAGE_UNKNOWN_NODE,
  STAGE_UNKNOWN_EDGE,
  STAGE_MISSING_STAGE,
  STAGE_DUPLICATE_STAGE,
  STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW,
  STAGE_INFEASIBLE_POSITIVE_CYCLE,
  STAGE_OUTPUT_ARITHMETIC_OVERFLOW,
  STAGE_FINAL_VERIFICATION_FAILED,
  STAGE_INTERNAL_ERROR,
};

struct StageSchedulingDiagnostic {
  StageSchedulingDiagnosticCode code = StageSchedulingDiagnosticCode::STAGE_INTERNAL_ERROR;
  std::string message;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
};

struct StageSchedulingStats {
  std::uint64_t constraints = 0;
  std::uint64_t relaxationRounds = 0;
  std::uint64_t successfulRelaxations = 0;
  std::uint64_t maxStage = 0;
  std::uint64_t maxLogicalIssueTime = 0;
};

struct StageConstraintCycleWitness {
  std::vector<cgra::target::TargetNodeId> nodes;
  std::vector<cgra::target::TargetEdgeId> edges;
  std::int64_t totalMinimumStageDelta = 0;
};

struct StageSchedulingResult {
  StageSchedulingStatus status = StageSchedulingStatus::InternalError;
  std::optional<StagedMapping> mapping;
  StageSchedulingStats stats;
  std::optional<StageConstraintCycleWitness> witness;
  std::vector<StageSchedulingDiagnostic> diagnostics;

  bool ok() const noexcept {
    return status == StageSchedulingStatus::Success && mapping.has_value();
  }
  std::string format() const;
  std::string toJson() const;
};

std::string_view toString(StageSchedulingStatus status) noexcept;
std::string_view toString(StageSchedulingDiagnosticCode code) noexcept;

} // namespace cgra::schedule
