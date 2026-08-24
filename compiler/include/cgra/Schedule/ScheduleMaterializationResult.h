// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/MaterializedSchedule.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::schedule {

enum class ScheduleMaterializationStatus {
  Success,
  InvalidTargetDFG,
  InvalidRFAllocatedMapping,
  InvalidTripCount,
  MissingRecurrenceBoundaryValue,
  BoundaryTypeMismatch,
  ArithmeticOverflow,
  MaterializationBudgetExceeded,
  PhaseFactorizationError,
  VerificationFailure,
  InternalError,
};

enum class ScheduleMaterializationDiagnosticCode : std::uint32_t {
  MAT_INVALID_TARGET_DFG,
  MAT_INVALID_RF_MAPPING,
  MAT_INVALID_TRIP_COUNT,
  MAT_BOUNDARY_VALUE_MISSING,
  MAT_BOUNDARY_VALUE_TYPE_MISMATCH,
  MAT_TIME_ARITHMETIC_OVERFLOW,
  MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED,
  MAT_PHASE_FACTORIZATION_FAILED,
  MAT_DUPLICATE_EVENT,
  MAT_MISSING_EVENT,
  MAT_INVALID_EVENT_PROVENANCE,
  MAT_FINAL_VERIFICATION_FAILED,
  MAT_INTERNAL_ERROR,
};

struct ScheduleMaterializationBudget {
  std::uint64_t maxExplicitBoundaryCycles = 1'000'000;
  std::uint64_t maxExplicitBoundaryEvents = 1'000'000;
};

struct ScheduleMaterializationRequest {
  std::uint64_t tripCount = 0;
  ScheduleMaterializationBudget budget;
};

struct ScheduleMaterializationDiagnostic {
  ScheduleMaterializationDiagnosticCode code =
      ScheduleMaterializationDiagnosticCode::MAT_INTERNAL_ERROR;
  std::string message;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<cgra::register_allocation::StorageSegmentId> segment;
  std::optional<std::int64_t> logicalIteration;
};

struct ScheduleMaterializationStats {
  std::uint64_t tripCount = 0;
  std::uint64_t periodicStreams = 0;
  std::uint64_t oneShotEvents = 0;
  std::uint64_t explicitPrologueCycles = 0;
  std::uint64_t explicitEpilogueCycles = 0;
  std::uint64_t kernelRepeatCount = 0;
  std::uint64_t explicitEvents = 0;
  std::uint64_t boundarySeedEvents = 0;
  std::uint64_t liveOutEvents = 0;
  std::uint64_t timeOriginShift = 0;
  std::uint64_t totalLogicalCycles = 0;
};

struct ScheduleMaterializationResult {
  ScheduleMaterializationStatus status = ScheduleMaterializationStatus::InternalError;
  std::optional<MaterializedSchedule> schedule;
  ScheduleMaterializationStats stats;
  std::vector<ScheduleMaterializationDiagnostic> diagnostics;

  bool ok() const noexcept {
    return status == ScheduleMaterializationStatus::Success && schedule.has_value();
  }
  std::string format() const;
  std::string toJson() const;
};

std::string_view toString(ScheduleMaterializationStatus status) noexcept;
std::string_view toString(ScheduleMaterializationDiagnosticCode code) noexcept;

} // namespace cgra::schedule
