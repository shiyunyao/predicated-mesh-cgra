// SPDX-License-Identifier: MIT
#include "cgra/Pipeline/BackendFeasibilityChecker.h"

#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Schedule/StageScheduler.h"

namespace cgra::pipeline {

mapping::CompleteMappingCheckResult
BackendFeasibilityChecker::check(const target::TargetDFG& dfg, const TargetModel& target,
                                 const mapping::ModuloMapping& mapping) const {
  const auto stage = schedule::StageScheduler::schedule(dfg, target, mapping);
  if (!stage.ok()) {
    if (stage.status == schedule::StageSchedulingStatus::InfeasibleStageConstraints)
      return {mapping::CompleteMappingDecision::Reject, "stage_infeasible", stage.format()};
    return {mapping::CompleteMappingDecision::Abort, "verification_stage_failure", stage.format()};
  }
  const auto stageReport = schedule::StageAssignmentVerifier::verify(dfg, target, *stage.mapping);
  if (!stageReport.ok())
    return {mapping::CompleteMappingDecision::Abort, "verification_stage_failure",
            stageReport.format()};

  const auto rf =
      register_allocation::RFAllocator::allocate(dfg, target, *stage.mapping, rfOptions_);
  switch (rf.status) {
  case register_allocation::RFAllocationStatus::FixedRegisterSelfOverlap:
  case register_allocation::RFAllocationStatus::ReadPortConflict:
  case register_allocation::RFAllocationStatus::WritePortConflict:
  case register_allocation::RFAllocationStatus::SameAddressRWConflict:
  case register_allocation::RFAllocationStatus::RegisterDepthInfeasible:
    return {mapping::CompleteMappingDecision::Reject, "rf_infeasible", rf.format()};
  case register_allocation::RFAllocationStatus::BudgetExceeded:
    return {mapping::CompleteMappingDecision::Abort, "budget_rf", rf.format()};
  case register_allocation::RFAllocationStatus::Success:
    break;
  default:
    return {mapping::CompleteMappingDecision::Abort, "verification_rf_failure", rf.format()};
  }
  const auto rfReport = register_allocation::RFAllocationVerifier::verify(dfg, target, *rf.mapping);
  if (!rfReport.ok())
    return {mapping::CompleteMappingDecision::Abort, "verification_rf_failure", rfReport.format()};
  return {mapping::CompleteMappingDecision::Accept, "", ""};
}

} // namespace cgra::pipeline
