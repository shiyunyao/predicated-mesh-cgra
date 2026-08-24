// SPDX-License-Identifier: MIT
#include "cgra/Schedule/ScheduleMaterializationResult.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace cgra::schedule {

std::string_view toString(ScheduleMaterializationStatus status) noexcept {
  switch (status) {
  case ScheduleMaterializationStatus::Success:
    return "success";
  case ScheduleMaterializationStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case ScheduleMaterializationStatus::InvalidRFAllocatedMapping:
    return "invalid_rf_allocated_mapping";
  case ScheduleMaterializationStatus::InvalidTripCount:
    return "invalid_trip_count";
  case ScheduleMaterializationStatus::MissingRecurrenceBoundaryValue:
    return "missing_recurrence_boundary_value";
  case ScheduleMaterializationStatus::BoundaryTypeMismatch:
    return "boundary_type_mismatch";
  case ScheduleMaterializationStatus::ArithmeticOverflow:
    return "arithmetic_overflow";
  case ScheduleMaterializationStatus::MaterializationBudgetExceeded:
    return "materialization_budget_exceeded";
  case ScheduleMaterializationStatus::PhaseFactorizationError:
    return "phase_factorization_error";
  case ScheduleMaterializationStatus::VerificationFailure:
    return "verification_failure";
  case ScheduleMaterializationStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(ScheduleMaterializationDiagnosticCode code) noexcept {
  switch (code) {
  case ScheduleMaterializationDiagnosticCode::MAT_INVALID_TARGET_DFG:
    return "MAT_INVALID_TARGET_DFG";
  case ScheduleMaterializationDiagnosticCode::MAT_INVALID_RF_MAPPING:
    return "MAT_INVALID_RF_MAPPING";
  case ScheduleMaterializationDiagnosticCode::MAT_INVALID_TRIP_COUNT:
    return "MAT_INVALID_TRIP_COUNT";
  case ScheduleMaterializationDiagnosticCode::MAT_BOUNDARY_VALUE_MISSING:
    return "MAT_BOUNDARY_VALUE_MISSING";
  case ScheduleMaterializationDiagnosticCode::MAT_BOUNDARY_VALUE_TYPE_MISMATCH:
    return "MAT_BOUNDARY_VALUE_TYPE_MISMATCH";
  case ScheduleMaterializationDiagnosticCode::MAT_TIME_ARITHMETIC_OVERFLOW:
    return "MAT_TIME_ARITHMETIC_OVERFLOW";
  case ScheduleMaterializationDiagnosticCode::MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED:
    return "MAT_EXPLICIT_BOUNDARY_BUDGET_EXCEEDED";
  case ScheduleMaterializationDiagnosticCode::MAT_PHASE_FACTORIZATION_FAILED:
    return "MAT_PHASE_FACTORIZATION_FAILED";
  case ScheduleMaterializationDiagnosticCode::MAT_DUPLICATE_EVENT:
    return "MAT_DUPLICATE_EVENT";
  case ScheduleMaterializationDiagnosticCode::MAT_MISSING_EVENT:
    return "MAT_MISSING_EVENT";
  case ScheduleMaterializationDiagnosticCode::MAT_INVALID_EVENT_PROVENANCE:
    return "MAT_INVALID_EVENT_PROVENANCE";
  case ScheduleMaterializationDiagnosticCode::MAT_FINAL_VERIFICATION_FAILED:
    return "MAT_FINAL_VERIFICATION_FAILED";
  case ScheduleMaterializationDiagnosticCode::MAT_INTERNAL_ERROR:
    return "MAT_INTERNAL_ERROR";
  }
  return "MAT_INTERNAL_ERROR";
}

std::string ScheduleMaterializationResult::format() const {
  std::ostringstream output;
  output << "ScheduleMaterialization status=" << toString(status)
         << " trip_count=" << stats.tripCount << " kernel_repeats=" << stats.kernelRepeatCount
         << " shift=" << stats.timeOriginShift << '\n';
  for (const auto& diagnostic : diagnostics) {
    output << "  [" << toString(diagnostic.code) << "] " << diagnostic.message;
    if (diagnostic.node)
      output << " node=" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    if (diagnostic.segment)
      output << " segment=" << *diagnostic.segment;
    output << '\n';
  }
  return output.str();
}

std::string ScheduleMaterializationResult::toJson() const {
  nlohmann::json root = {{"schema", "cgra.schedule_materialization.result.v1"},
                         {"status", toString(status)},
                         {"ok", ok()},
                         {"ii", schedule ? schedule->ii() : 0U},
                         {"stats",
                          {{"trip_count", stats.tripCount},
                           {"periodic_streams", stats.periodicStreams},
                           {"one_shot_events", stats.oneShotEvents},
                           {"prologue_cycles", stats.explicitPrologueCycles},
                           {"epilogue_cycles", stats.explicitEpilogueCycles},
                           {"kernel_repeat_count", stats.kernelRepeatCount},
                           {"explicit_events", stats.explicitEvents},
                           {"boundary_seed_events", stats.boundarySeedEvents},
                           {"live_out_events", stats.liveOutEvents},
                           {"time_origin_shift", stats.timeOriginShift},
                           {"total_logical_cycles", stats.totalLogicalCycles}}},
                         {"diagnostics", nlohmann::json::array()}};
  for (const auto& diagnostic : diagnostics) {
    nlohmann::json item = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.node)
      item["node"] = *diagnostic.node;
    if (diagnostic.edge)
      item["edge"] = *diagnostic.edge;
    if (diagnostic.segment)
      item["segment"] = *diagnostic.segment;
    root["diagnostics"].push_back(std::move(item));
  }
  return root.dump(2);
}

} // namespace cgra::schedule
