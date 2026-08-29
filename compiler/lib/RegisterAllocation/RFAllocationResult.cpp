// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocationResult.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace cgra::register_allocation {

std::string_view toString(RFAllocationStatus status) noexcept {
  switch (status) {
  case RFAllocationStatus::Success:
    return "success";
  case RFAllocationStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case RFAllocationStatus::InvalidStagedMapping:
    return "invalid_staged_mapping";
  case RFAllocationStatus::TargetRFContractError:
    return "target_rf_contract_error";
  case RFAllocationStatus::FixedRegisterSelfOverlap:
    return "fixed_register_self_overlap";
  case RFAllocationStatus::ReadPortConflict:
    return "read_port_conflict";
  case RFAllocationStatus::WritePortConflict:
    return "write_port_conflict";
  case RFAllocationStatus::SameAddressRWConflict:
    return "same_address_rw_conflict";
  case RFAllocationStatus::RegisterDepthInfeasible:
    return "register_depth_infeasible";
  case RFAllocationStatus::RotationFactorOverflow:
    return "rotation_factor_overflow";
  case RFAllocationStatus::RotationPeriodExceedsControlMemory:
    return "rotation_period_exceeds_control_memory";
  case RFAllocationStatus::BudgetExceeded:
    return "budget_exceeded";
  case RFAllocationStatus::VerificationFailure:
    return "verification_failure";
  case RFAllocationStatus::ArithmeticOverflow:
    return "arithmetic_overflow";
  case RFAllocationStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(RFAllocationDiagnosticCode code) noexcept {
  switch (code) {
  case RFAllocationDiagnosticCode::RFA_INVALID_TARGET_DFG:
    return "RFA_INVALID_TARGET_DFG";
  case RFAllocationDiagnosticCode::RFA_INVALID_STAGED_MAPPING:
    return "RFA_INVALID_STAGED_MAPPING";
  case RFAllocationDiagnosticCode::RFA_TARGET_BANK_MISSING:
    return "RFA_TARGET_BANK_MISSING";
  case RFAllocationDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID:
    return "RFA_TARGET_RF_CONTRACT_INVALID";
  case RFAllocationDiagnosticCode::RFA_STORAGE_TIMING_INVALID:
    return "RFA_STORAGE_TIMING_INVALID";
  case RFAllocationDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW:
    return "RFA_STORAGE_ARITHMETIC_OVERFLOW";
  case RFAllocationDiagnosticCode::RFA_FIXED_REGISTER_SELF_OVERLAP:
    return "RFA_FIXED_REGISTER_SELF_OVERLAP";
  case RFAllocationDiagnosticCode::RFA_READ_PORT_CONFLICT:
    return "RFA_READ_PORT_CONFLICT";
  case RFAllocationDiagnosticCode::RFA_WRITE_PORT_CONFLICT:
    return "RFA_WRITE_PORT_CONFLICT";
  case RFAllocationDiagnosticCode::RFA_SAME_ADDRESS_RW_CONFLICT:
    return "RFA_SAME_ADDRESS_RW_CONFLICT";
  case RFAllocationDiagnosticCode::RFA_REGISTER_DEPTH_INFEASIBLE:
    return "RFA_REGISTER_DEPTH_INFEASIBLE";
  case RFAllocationDiagnosticCode::RFA_ROTATION_FACTOR_OVERFLOW:
    return "RFA_ROTATION_FACTOR_OVERFLOW";
  case RFAllocationDiagnosticCode::RFA_ROTATION_PERIOD_EXCEEDS_CONTROL_MEMORY:
    return "RFA_ROTATION_PERIOD_EXCEEDS_CONTROL_MEMORY";
  case RFAllocationDiagnosticCode::RFA_COLORING_BUDGET_EXCEEDED:
    return "RFA_COLORING_BUDGET_EXCEEDED";
  case RFAllocationDiagnosticCode::RFA_UNKNOWN_STORAGE_SEGMENT:
    return "RFA_UNKNOWN_STORAGE_SEGMENT";
  case RFAllocationDiagnosticCode::RFA_DUPLICATE_STORAGE_ALLOCATION:
    return "RFA_DUPLICATE_STORAGE_ALLOCATION";
  case RFAllocationDiagnosticCode::RFA_UNALLOCATED_STORAGE_SEGMENT:
    return "RFA_UNALLOCATED_STORAGE_SEGMENT";
  case RFAllocationDiagnosticCode::RFA_INVALID_REGISTER_INDEX:
    return "RFA_INVALID_REGISTER_INDEX";
  case RFAllocationDiagnosticCode::RFA_BANK_DOMAIN_MISMATCH:
    return "RFA_BANK_DOMAIN_MISMATCH";
  case RFAllocationDiagnosticCode::RFA_FINAL_VERIFICATION_FAILED:
    return "RFA_FINAL_VERIFICATION_FAILED";
  case RFAllocationDiagnosticCode::RFA_INTERNAL_ERROR:
    return "RFA_INTERNAL_ERROR";
  }
  return "RFA_INTERNAL_ERROR";
}

std::string RFAllocationResult::format() const {
  std::ostringstream output;
  output << "RFAllocation status=" << toString(status) << " segments=" << stats.storageSegments
         << " decisions=" << stats.coloringDecisions << '\n';
  for (const auto& diagnostic : diagnostics) {
    output << "  [" << toString(diagnostic.code) << "] " << diagnostic.message;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    if (diagnostic.segment)
      output << " segment=" << *diagnostic.segment;
    output << '\n';
  }
  return output.str();
}

std::string RFAllocationResult::toJson() const {
  nlohmann::json root = {{"schema", "cgra.rf_allocation.result.v1"},
                         {"status", toString(status)},
                         {"ok", ok()},
                         {"stats",
                          {{"storage_segments", stats.storageSegments},
                           {"data_segments", stats.dataSegments},
                           {"predicate_segments", stats.predicateSegments},
                           {"coloring_decisions", stats.coloringDecisions},
                           {"coloring_backtracks", stats.coloringBacktracks},
                           {"max_registers_used", stats.maxRegistersUsedOnAnyBank},
                           {"max_read_ports_used", stats.maxReadPortsUsed},
                           {"max_write_ports_used", stats.maxWritePortsUsed}}},
                         {"diagnostics", nlohmann::json::array()}};
  for (const auto& diagnostic : diagnostics) {
    nlohmann::json value = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    if (diagnostic.segment)
      value["segment"] = *diagnostic.segment;
    root["diagnostics"].push_back(std::move(value));
  }
  return root.dump(2);
}

} // namespace cgra::register_allocation
