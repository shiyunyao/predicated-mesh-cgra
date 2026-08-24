// SPDX-License-Identifier: MIT
#include "cgra/Schedule/StageSchedulingResult.h"
#include "cgra/Schedule/StagedMappingSerialization.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace cgra::schedule {

std::string_view toString(StageSchedulingStatus status) noexcept {
  switch (status) {
  case StageSchedulingStatus::Success:
    return "success";
  case StageSchedulingStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case StageSchedulingStatus::InvalidModuloMapping:
    return "invalid_modulo_mapping";
  case StageSchedulingStatus::InfeasibleStageConstraints:
    return "infeasible_stage_constraints";
  case StageSchedulingStatus::ArithmeticOverflow:
    return "arithmetic_overflow";
  case StageSchedulingStatus::VerificationFailure:
    return "verification_failure";
  case StageSchedulingStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(StageSchedulingDiagnosticCode code) noexcept {
  switch (code) {
  case StageSchedulingDiagnosticCode::STAGE_INVALID_TARGET_DFG:
    return "STAGE_INVALID_TARGET_DFG";
  case StageSchedulingDiagnosticCode::STAGE_INVALID_MODULO_MAPPING:
    return "STAGE_INVALID_MODULO_MAPPING";
  case StageSchedulingDiagnosticCode::STAGE_UNKNOWN_NODE:
    return "STAGE_UNKNOWN_NODE";
  case StageSchedulingDiagnosticCode::STAGE_UNKNOWN_EDGE:
    return "STAGE_UNKNOWN_EDGE";
  case StageSchedulingDiagnosticCode::STAGE_MISSING_STAGE:
    return "STAGE_MISSING_STAGE";
  case StageSchedulingDiagnosticCode::STAGE_DUPLICATE_STAGE:
    return "STAGE_DUPLICATE_STAGE";
  case StageSchedulingDiagnosticCode::STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW:
    return "STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW";
  case StageSchedulingDiagnosticCode::STAGE_INFEASIBLE_POSITIVE_CYCLE:
    return "STAGE_INFEASIBLE_POSITIVE_CYCLE";
  case StageSchedulingDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW:
    return "STAGE_OUTPUT_ARITHMETIC_OVERFLOW";
  case StageSchedulingDiagnosticCode::STAGE_FINAL_VERIFICATION_FAILED:
    return "STAGE_FINAL_VERIFICATION_FAILED";
  case StageSchedulingDiagnosticCode::STAGE_INTERNAL_ERROR:
    return "STAGE_INTERNAL_ERROR";
  }
  return "STAGE_INTERNAL_ERROR";
}

std::string StageSchedulingResult::format() const {
  std::ostringstream output;
  output << "StageScheduling status=" << toString(status) << " constraints=" << stats.constraints
         << " rounds=" << stats.relaxationRounds << " max_stage=" << stats.maxStage << '\n';
  for (const auto& diagnostic : diagnostics) {
    output << "  [" << toString(diagnostic.code) << "] " << diagnostic.message;
    if (diagnostic.node)
      output << " node=" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    output << '\n';
  }
  if (witness) {
    output << "  witness delta=" << witness->totalMinimumStageDelta << " nodes=";
    for (const auto node : witness->nodes)
      output << node << ' ';
    output << "edges=";
    for (const auto edge : witness->edges)
      output << edge << ' ';
    output << '\n';
  }
  return output.str();
}

std::string StageSchedulingResult::toJson() const {
  nlohmann::json root = {{"schema", "cgra.stage_scheduling.result.v1"},
                         {"status", toString(status)},
                         {"ok", ok()},
                         {"stats",
                          {{"constraints", stats.constraints},
                           {"relaxation_rounds", stats.relaxationRounds},
                           {"successful_relaxations", stats.successfulRelaxations},
                           {"max_stage", stats.maxStage},
                           {"max_logical_issue_time", stats.maxLogicalIssueTime}}},
                         {"diagnostics", nlohmann::json::array()}};
  for (const auto& diagnostic : diagnostics) {
    nlohmann::json value = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.node)
      value["node"] = *diagnostic.node;
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    root["diagnostics"].push_back(std::move(value));
  }
  if (witness) {
    root["witness"] = {{"nodes", witness->nodes},
                       {"edges", witness->edges},
                       {"total_minimum_stage_delta", witness->totalMinimumStageDelta}};
  }
  if (mapping)
    root["mapping"] = nlohmann::json::parse(StagedMappingSerialization::toJson(*mapping));
  return root.dump(2);
}

} // namespace cgra::schedule
