// SPDX-License-Identifier: MIT
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Transforms/RecurrenceIngressNormalization.h"
#include "cgra/Transforms/RecurrenceIngressVerifier.h"

#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"
#include "cgra/Lowering/TargetLowering.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Pipeline/BackendFeasibilityChecker.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/Schedule/MaterializedScheduleSerialization.h"
#include "cgra/Schedule/MaterializedScheduleVerifier.h"
#include "cgra/Schedule/ScheduleMaterializer.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Schedule/StagedMappingSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetDFGVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cgra::pipeline {
namespace {

using Json = nlohmann::json;

class ArtifactWriter {
public:
  ArtifactWriter(std::filesystem::path directory, CompileDFGResult& result)
      : directory_(std::move(directory)), result_(result) {
    if (!directory_.empty())
      std::filesystem::create_directories(directory_);
  }

  void write(std::string_view name, std::string_view contents) {
    if (directory_.empty())
      return;
    const auto finalPath = directory_ / name;
    const auto temporary = finalPath.string() + ".tmp";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output)
        throw std::runtime_error("cannot write compiler artifact " + temporary);
      output << contents;
      if (contents.empty() || contents.back() != '\n')
        output << '\n';
    }
    std::filesystem::rename(temporary, finalPath);
    result_.artifacts.push_back(finalPath);
  }

  const std::filesystem::path& directory() const noexcept { return directory_; }

private:
  std::filesystem::path directory_;
  CompileDFGResult& result_;
};

Json verification(bool ok, std::string_view detail = {}) {
  return Json{{"ok", ok}, {"detail", detail}};
}

CompileDFGResult failure(CompileDFGResult& result, CompileDFGStatus status, std::string message,
                         ArtifactWriter& artifacts) {
  result.status = status;
  result.message = std::move(message);
  artifacts.write("compiler_pipeline_report.json", result.toJson());
  return result;
}

} // namespace

std::string_view toString(CompileDFGStatus status) noexcept {
  switch (status) {
  case CompileDFGStatus::Success:
    return "success";
  case CompileDFGStatus::GenericDFGVerificationFailure:
    return "generic_dfg_verification_failure";
  case CompileDFGStatus::TargetLegalizationFailure:
    return "target_legalization_failure";
  case CompileDFGStatus::TargetDFGVerificationFailure:
    return "target_dfg_verification_failure";
  case CompileDFGStatus::MIIAnalysisFailure:
    return "mii_analysis_failure";
  case CompileDFGStatus::MappingFailure:
    return "mapping_failure";
  case CompileDFGStatus::RFConstrainedMappingFailure:
    return "rf_constrained_mapping_failure";
  case CompileDFGStatus::RFConstrainedMappingBudgetFailure:
    return "rf_constrained_mapping_budget_failure";
  case CompileDFGStatus::ModuloMappingVerificationFailure:
    return "modulo_mapping_verification_failure";
  case CompileDFGStatus::StageSchedulingFailure:
    return "stage_scheduling_failure";
  case CompileDFGStatus::StageVerificationFailure:
    return "stage_verification_failure";
  case CompileDFGStatus::RFAllocationFailure:
    return "rf_allocation_failure";
  case CompileDFGStatus::RFVerificationFailure:
    return "rf_verification_failure";
  case CompileDFGStatus::MaterializationFailure:
    return "materialization_failure";
  case CompileDFGStatus::MaterializedScheduleVerificationFailure:
    return "materialized_schedule_verification_failure";
  case CompileDFGStatus::TargetLoweringFailure:
    return "target_lowering_failure";
  case CompileDFGStatus::TargetControlVerificationFailure:
    return "target_control_verification_failure";
  case CompileDFGStatus::ManifestValidationFailure:
    return "manifest_validation_failure";
  case CompileDFGStatus::ArtifactFailure:
    return "artifact_failure";
  case CompileDFGStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(CompileDFGMode mode) noexcept {
  switch (mode) {
  case CompileDFGMode::HardwareExecutable:
    return "hardware_executable";
  case CompileDFGMode::MappingResearch:
    return "mapping_research";
  }
  return "hardware_executable";
}

std::string_view toString(PhysicalRealizabilityStatus status) noexcept {
  switch (status) {
  case PhysicalRealizabilityStatus::NotRun:
    return "not_run";
  case PhysicalRealizabilityStatus::Feasible:
    return "feasible";
  case PhysicalRealizabilityStatus::Infeasible:
    return "infeasible";
  case PhysicalRealizabilityStatus::Error:
    return "error";
  }
  return "error";
}

std::string CompileDFGResult::toJson() const {
  Json value{{"schema", "cgra.compiler_pipeline.result.v1"},
             {"mode", toString(mode)},
             {"status", toString(status)},
             {"message", message},
             {"mapping_status", moduloMapping ? "rf_constrained_success" :
                                               (stats.rfBudgetExceeded ? "route_mapped_rf_budget" :
                                                (stats.completedModuloMappings ? "route_mapped_rf_infeasible"
                                                                                : "not_available"))},
             {"raw_mapping_found", stats.completedModuloMappings != 0},
             {"rf_constrained_mapping_found", moduloMapping.has_value()},
             {"hardware_executable", hardwareExecutable()},
             {"physical_realizability",
              {{"status", toString(physicalRealizability.status)},
               {"reason_code", physicalRealizability.reasonCode},
               {"message", physicalRealizability.message}}},
             {"stats",
               {{"trip_count", stats.tripCount},
               {"mii", stats.mii},
               {"mapper_invoked", stats.mapperInvoked},
               {"mapped_ii", stats.mappedII},
               {"safe_ii", stats.safeII},
               {"best_known_ii", stats.bestKnownII},
               {"mapping_solution_kind", stats.mappingSolutionKind},
               {"feasibility_fallback_invoked", stats.feasibilityFallbackInvoked},
               {"feasibility_fallback_attempts", stats.feasibilityFallbackAttempts},
               {"feasibility_fallback_schedule_growth",
                stats.feasibilityFallbackScheduleGrowth},
               {"suppressed_mapper_diagnostics", stats.suppressedMapperDiagnostics},
               {"node_candidate_attempts", stats.nodeCandidateAttempts},
               {"route_state_expansions", stats.routeStateExpansions},
               {"completed_modulo_mappings", stats.completedModuloMappings},
               {"post_mapping_rejected", stats.postMappingRejected},
               {"stage_rejected", stats.stageRejected},
               {"rf_rejected", stats.rfRejected},
               {"rf_port_match_calls", stats.rfPortMatchCalls},
               {"rf_port_match_failures", stats.rfPortMatchFailures},
               {"rf_read_port_early_rejects", stats.rfReadPortEarlyRejects},
               {"rf_write_port_early_rejects", stats.rfWritePortEarlyRejects},
               {"rf_write_source_early_rejects", stats.rfWriteSourceEarlyRejects},
               {"rf_port_events_committed", stats.rfPortEventsCommitted},
               {"rf_port_rollback_count", stats.rfPortRollbackCount},
               {"late_read_port_conflicts", stats.lateReadPortConflicts},
               {"late_write_port_conflicts", stats.lateWritePortConflicts},
               {"rf_budget_exceeded", stats.rfBudgetExceeded},
               {"rf_rejected_by_ii", Json::object()},
               {"rf_rejected_by_reason", Json::object()},
               {"rf_constrained_mappings", stats.rfConstrainedMappings},
               {"post_mapping_abort", stats.postMappingAbort},
               {"max_stage", stats.maxStage},
               {"storage_segments", stats.storageSegments},
               {"rotation_families", stats.rotationFamilies},
               {"max_rotation_factor", stats.maxRotationFactor},
               {"rotation_period_iterations", stats.rotationPeriodIterations},
               {"control_period_cycles", stats.controlPeriodCycles},
               {"prologue_cycles", stats.prologueCycles},
               {"kernel_repeats", stats.kernelRepeats},
               {"epilogue_cycles", stats.epilogueCycles}}},
             {"artifacts", Json::array()}};
  for (const auto &[ii, count] : stats.rfRejectedByII)
    value["stats"]["rf_rejected_by_ii"][std::to_string(ii)] = count;
  for (const auto &[reason, count] : stats.rfRejectedByReason)
    value["stats"]["rf_rejected_by_reason"][reason] = count;
  for (const auto& artifact : artifacts)
    value["artifacts"].push_back(artifact.string());
  return value.dump(2);
}

CompileDFGResult compileGenericDFG(const ir::DFG& dfg, const TargetModel& target,
                                   const CompileDFGOptions& options) {
  CompileDFGResult result;
  try {
    result.mode = options.mode;
    ArtifactWriter artifacts(options.artifactDirectory, result);
    result.stats.tripCount = options.tripCount;
    artifacts.write("00_input.generic_dfg.json", ir::toJson(dfg));

    if (options.mode == CompileDFGMode::HardwareExecutable && target.isMappingResearchTarget())
      return failure(result, CompileDFGStatus::TargetLegalizationFailure,
                     "TARGET_MAPPING_PROFILE_NOT_HARDWARE_EXECUTABLE: abstract mapping target "
                     "cannot enter the hardware-executable pipeline",
                     artifacts);

    const auto genericReport = ir::DFGVerifier::verify(dfg);
    artifacts.write("01_generic_dfg_verification.json", genericReport.toJson());
    if (!genericReport.ok())
      return failure(result, CompileDFGStatus::GenericDFGVerificationFailure,
                     genericReport.format(), artifacts);

    auto normalized = options.normalizeRecurrenceIngress
                          ? transforms::normalizeRecurrenceIngress(dfg)
                          : transforms::RecurrenceIngressNormalizationResult{dfg, {}, {}};
    if (options.normalizeRecurrenceIngress && normalized.changed()) {
      Json ingress = {{"schema", "cgra.recurrence_ingress_normalization.v1"}};
      ingress["records"] = Json::array();
      for (const auto& record : normalized.records) {
        ingress["records"].push_back({
            {"original_edge", record.originalEdge},
            {"original_source", record.originalSource},
            {"original_destination", record.originalDestination},
            {"original_destination_operand", record.originalDestinationOperand},
            {"ingress_node", record.ingressNode},
            {"recurrence_edge", record.recurrenceEdge},
            {"local_edge", record.localEdge},
            {"kind", record.kind == transforms::RecurrenceIngressKind::Predicate ? "predicate"
                                                                                     : "data"},
            {"value_type", record.valueType.toString()},
            {"distance", record.distance},
            {"boundary_hash", record.boundaryHash},
            {"source_recurrence_provenance", record.sourceRecurrenceProvenance},
            {"consumer_count", record.consumerCount},
            {"shared_ingress", record.sharedIngress}});
      }
      artifacts.write("01_recurrence_ingress_normalization.json", ingress.dump(2));
      const auto ingressReport = transforms::verifyRecurrenceIngress(dfg, normalized);
      artifacts.write("01_recurrence_ingress_normalization_verification.json",
                      Json{{"ok", ingressReport.ok}, {"detail", ingressReport.format()}}.dump(2));
      if (!ingressReport.ok)
        return failure(result, CompileDFGStatus::GenericDFGVerificationFailure,
                       ingressReport.format(), artifacts);
      const auto normalizedReport = ir::DFGVerifier::verify(normalized.dfg);
      artifacts.write("01_recurrence_ingress_verification.json", normalizedReport.toJson());
      if (!normalizedReport.ok())
        return failure(result, CompileDFGStatus::GenericDFGVerificationFailure,
                       normalizedReport.format(), artifacts);
    }
    const auto& semanticDfg = normalized.dfg;

    const auto legalization = target::TargetLegalizer::legalize(semanticDfg, target);
    artifacts.write("02_legalization.json", legalization.toJson());
    if (!legalization.ok())
      return failure(result, CompileDFGStatus::TargetLegalizationFailure, legalization.format(),
                     artifacts);
    const auto& targetDFG = *legalization.dfg;
    artifacts.write("03_target_dfg.json", target::toJson(targetDFG));
    const auto targetReport = target::TargetDFGVerifier::verify(targetDFG, target, &semanticDfg);
    artifacts.write("04_target_dfg_verification.json", targetReport.toJson());
    if (!targetReport.ok())
      return failure(result, CompileDFGStatus::TargetDFGVerificationFailure, targetReport.format(),
                     artifacts);

    const auto mii = analysis::MIIAnalyzer::analyze(targetDFG, target);
    artifacts.write("05_mii.json", mii.toJson());
    if (!mii.ok())
      return failure(result, CompileDFGStatus::MIIAnalysisFailure, mii.format(), artifacts);
    result.stats.mii = mii.mii;

    auto mapperOptions = options.mapper;
    BackendFeasibilityChecker completionChecker(options.rfAllocation);
    if (options.mode == CompileDFGMode::HardwareExecutable ||
        options.mode == CompileDFGMode::MappingResearch) {
      mapperOptions.completeMappingChecker =
          [&completionChecker](const target::TargetDFG& candidateDFG,
                               const TargetModel& candidateTarget,
                               const mapping::ModuloMapping& candidate) {
            return completionChecker.check(candidateDFG, candidateTarget, candidate);
          };
    } else {
      mapperOptions.completeMappingChecker = {};
    }
    result.stats.mapperInvoked = true;
    const auto mapped = mapping::ModuloMapper::map(targetDFG, target, mapperOptions);
    artifacts.write("06_mapper_report.json", mapped.toJson());
    result.stats.nodeCandidateAttempts = mapped.stats.nodeCandidateAttempts;
    result.stats.routeStateExpansions = mapped.stats.totalRouteStateExpansions;
    result.stats.completedModuloMappings = mapped.stats.completedModuloMappings;
    result.stats.postMappingRejected = mapped.stats.postMappingRejected;
    result.stats.stageRejected = mapped.stats.stageRejected;
    result.stats.rfRejected = mapped.stats.rfRejected;
    result.stats.rfPortMatchCalls = mapped.stats.rfPortMatchCalls;
    result.stats.rfPortMatchFailures = mapped.stats.rfPortMatchFailures;
    result.stats.rfReadPortEarlyRejects = mapped.stats.rfReadPortEarlyRejects;
    result.stats.rfWritePortEarlyRejects = mapped.stats.rfWritePortEarlyRejects;
    result.stats.rfWriteSourceEarlyRejects = mapped.stats.rfWriteSourceEarlyRejects;
    result.stats.rfPortEventsCommitted = mapped.stats.rfPortEventsCommitted;
    result.stats.rfPortRollbackCount = mapped.stats.rfPortRollbackCount;
    result.stats.lateReadPortConflicts = mapped.stats.lateReadPortConflicts;
    result.stats.lateWritePortConflicts = mapped.stats.lateWritePortConflicts;
    result.stats.rfBudgetExceeded = mapped.stats.rfBudgetExceeded;
    result.stats.rfRejectedByII = mapped.stats.rfRejectedByII;
    result.stats.rfRejectedByReason = mapped.stats.rfRejectedByReason;
    result.stats.postMappingAbort = mapped.stats.postMappingAbort;
    result.stats.safeII = mapped.safeII;
    result.stats.bestKnownII = mapped.bestKnownII;
    result.stats.mappingSolutionKind = mapped.solutionKind;
    result.stats.feasibilityFallbackInvoked = mapped.fallbackInvoked;
    result.stats.feasibilityFallbackAttempts = mapped.fallbackAttempts;
    result.stats.feasibilityFallbackScheduleGrowth = mapped.fallbackScheduleGrowth;
    result.stats.suppressedMapperDiagnostics = mapped.suppressedDiagnostics;
    if (!mapped.ok()) {
      const auto status = options.mode == CompileDFGMode::MappingResearch &&
                                  mapped.stats.rfBudgetExceeded != 0
                              ? CompileDFGStatus::RFConstrainedMappingBudgetFailure
                              : options.mode == CompileDFGMode::MappingResearch &&
                                  mapped.stats.completedModuloMappings != 0 &&
                                  mapped.stats.rfRejected != 0
                              ? CompileDFGStatus::RFConstrainedMappingFailure
                              : CompileDFGStatus::MappingFailure;
      if (status == CompileDFGStatus::RFConstrainedMappingFailure) {
        result.physicalRealizability.status = PhysicalRealizabilityStatus::Error;
        result.physicalRealizability.reasonCode = "only_rf_invalid_candidates_found";
        result.physicalRealizability.message = mapped.format();
      } else if (status == CompileDFGStatus::RFConstrainedMappingBudgetFailure) {
        result.physicalRealizability.status = PhysicalRealizabilityStatus::Error;
        result.physicalRealizability.reasonCode = "rf_budget";
        result.physicalRealizability.message = mapped.format();
      }
      const auto message = status == CompileDFGStatus::RFConstrainedMappingBudgetFailure
                               ? "RF_BUDGET: " + mapped.format()
                               : mapped.format();
      return failure(result, status, message, artifacts);
    }
    result.stats.mappedII = mapped.mapping->ii();
    artifacts.write("07_modulo_mapping.json", mapping::toJson(*mapped.mapping));
    const auto mappingReport =
        mapping::ModuloMappingVerifier::verify(targetDFG, target, *mapped.mapping);
    artifacts.write("08_modulo_mapping_verification.json", mappingReport.toJson());
    if (!mappingReport.ok())
      return failure(result, CompileDFGStatus::ModuloMappingVerificationFailure,
                     mappingReport.format(), artifacts);

    if (options.mode == CompileDFGMode::MappingResearch) {
      // The mapper completion callback already checked these invariants while
      // searching. Re-run them for the accepted candidate so the research
      // artifact contains the same independent stage/RF evidence as the
      // hardware lane, without entering materialization or lowering.
      const auto staged = schedule::StageScheduler::schedule(targetDFG, target, *mapped.mapping);
      artifacts.write("09_stage_report.json", staged.toJson());
      if (!staged.ok())
        return failure(result, CompileDFGStatus::StageSchedulingFailure, staged.format(), artifacts);
      result.stats.maxStage = staged.mapping->maxStage();
      artifacts.write("10_staged_mapping.json", schedule::toJson(*staged.mapping));
      const auto stageReport =
          schedule::StageAssignmentVerifier::verify(targetDFG, target, *staged.mapping);
      artifacts.write("11_stage_verification.json",
                      verification(stageReport.ok(), stageReport.format()).dump(2));
      if (!stageReport.ok())
        return failure(result, CompileDFGStatus::StageVerificationFailure, stageReport.format(),
                       artifacts);
      const auto allocated = register_allocation::RFAllocator::allocate(
          targetDFG, target, *staged.mapping, options.rfAllocation);
      artifacts.write("12_rf_report.json", allocated.toJson());
      if (!allocated.ok())
        return failure(result, CompileDFGStatus::RFAllocationFailure, allocated.format(), artifacts);
      result.stats.storageSegments = allocated.mapping->storageRequirements().segments().size();
      result.stats.rotationFamilies = std::count_if(
          allocated.mapping->allocations().begin(), allocated.mapping->allocations().end(),
          [](const auto& allocation) { return allocation.family.phaseCount > 1; });
      result.stats.maxRotationFactor = 1;
      for (const auto& allocation : allocated.mapping->allocations())
        result.stats.maxRotationFactor =
            std::max(result.stats.maxRotationFactor, allocation.family.phaseCount);
      try {
        result.stats.rotationPeriodIterations = allocated.mapping->rotationPeriodIterations();
        result.stats.controlPeriodCycles =
            allocated.mapping->controlPeriodCycles(mapped.mapping->ii());
      } catch (const std::overflow_error& error) {
        return failure(result, CompileDFGStatus::RFVerificationFailure, error.what(), artifacts);
      }
      artifacts.write("13_rf_allocated_mapping.json",
                      register_allocation::toJson(*allocated.mapping));
      const auto rfReport =
          register_allocation::RFAllocationVerifier::verify(targetDFG, target, *allocated.mapping);
      artifacts.write("14_rf_verification.json",
                      verification(rfReport.ok(), rfReport.format()).dump(2));
      if (!rfReport.ok())
        return failure(result, CompileDFGStatus::RFVerificationFailure, rfReport.format(), artifacts);
      result.moduloMapping = mapped.mapping;
      result.stats.rfConstrainedMappings = 1;
      result.physicalRealizability.status = PhysicalRealizabilityStatus::Feasible;
      result.physicalRealizability.reasonCode = "rf_constrained_mapping_accepted";
      result.physicalRealizability.message =
          "stage assignment and finite register allocation accepted the mapping";
      result.status = CompileDFGStatus::Success;
      result.message = "RF-constrained modulo mapping produced";
      artifacts.write("09_physical_realizability.json",
                      Json{{"schema", "cgra.physical_realizability.v1"},
                           {"status", toString(result.physicalRealizability.status)},
                           {"reason_code", result.physicalRealizability.reasonCode},
                           {"message", result.physicalRealizability.message}}
                          .dump(2));
      artifacts.write("compiler_pipeline_report.json", result.toJson());
      return result;
    }

    const auto staged = schedule::StageScheduler::schedule(targetDFG, target, *mapped.mapping);
    artifacts.write("09_stage_report.json", staged.toJson());
    if (!staged.ok())
      return failure(result, CompileDFGStatus::StageSchedulingFailure, staged.format(), artifacts);
    result.stats.maxStage = staged.mapping->maxStage();
    artifacts.write("10_staged_mapping.json", schedule::toJson(*staged.mapping));
    const auto stageReport =
        schedule::StageAssignmentVerifier::verify(targetDFG, target, *staged.mapping);
    artifacts.write("11_stage_verification.json",
                    verification(stageReport.ok(), stageReport.format()).dump(2));
    if (!stageReport.ok())
      return failure(result, CompileDFGStatus::StageVerificationFailure, stageReport.format(),
                     artifacts);

    const auto allocated = register_allocation::RFAllocator::allocate(
        targetDFG, target, *staged.mapping, options.rfAllocation);
    artifacts.write("12_rf_report.json", allocated.toJson());
    if (!allocated.ok())
      return failure(result, CompileDFGStatus::RFAllocationFailure, allocated.format(), artifacts);
    result.stats.storageSegments = allocated.mapping->storageRequirements().segments().size();
    result.stats.rotationFamilies = std::count_if(
        allocated.mapping->allocations().begin(), allocated.mapping->allocations().end(),
        [](const auto& allocation) { return allocation.family.phaseCount > 1; });
    result.stats.maxRotationFactor = 1;
    for (const auto& allocation : allocated.mapping->allocations())
      result.stats.maxRotationFactor =
          std::max(result.stats.maxRotationFactor, allocation.family.phaseCount);
    try {
      result.stats.rotationPeriodIterations = allocated.mapping->rotationPeriodIterations();
      result.stats.controlPeriodCycles =
          allocated.mapping->controlPeriodCycles(mapped.mapping->ii());
    } catch (const std::overflow_error& error) {
      return failure(result, CompileDFGStatus::RFVerificationFailure, error.what(), artifacts);
    }
    artifacts.write("13_rf_allocated_mapping.json",
                    register_allocation::toJson(*allocated.mapping));
    const auto rfReport =
        register_allocation::RFAllocationVerifier::verify(targetDFG, target, *allocated.mapping);
    artifacts.write("14_rf_verification.json",
                    verification(rfReport.ok(), rfReport.format()).dump(2));
    if (!rfReport.ok())
      return failure(result, CompileDFGStatus::RFVerificationFailure, rfReport.format(), artifacts);
    result.moduloMapping = mapped.mapping;

    schedule::ScheduleMaterializationRequest request{options.tripCount,
                                                     options.materializationBudget};
    const auto materialized =
        schedule::ScheduleMaterializer::materialize(targetDFG, target, *allocated.mapping, request);
    artifacts.write("15_materialization_report.json", materialized.toJson());
    if (!materialized.ok())
      return failure(result, CompileDFGStatus::MaterializationFailure, materialized.format(),
                     artifacts);
    result.stats.prologueCycles = materialized.schedule->prologue().cycles.size();
    result.stats.kernelRepeats = materialized.schedule->kernel().repeatCount;
    result.stats.epilogueCycles = materialized.schedule->epilogue().cycles.size();
    artifacts.write("16_materialized_schedule.json", schedule::toJson(*materialized.schedule));
    const auto materializedReport = schedule::MaterializedScheduleVerifier::verify(
        targetDFG, target, *allocated.mapping, request, *materialized.schedule);
    artifacts.write("17_materialization_verification.json",
                    verification(materializedReport.ok(), materializedReport.format()).dump(2));
    if (!materializedReport.ok())
      return failure(result, CompileDFGStatus::MaterializedScheduleVerificationFailure,
                     materializedReport.format(), artifacts);

    lowering::TargetLoweringOptions loweringOptions;
    loweringOptions.programName = options.programName;
    loweringOptions.targetPath = options.targetPath.string();
    loweringOptions.observation = "compiler E2E scratchpad stores";
    loweringOptions.scratchpadPreload = options.scratchpadPreload;
    const auto lowered = lowering::TargetLowering::lower(targetDFG, target, *allocated.mapping,
                                                         *materialized.schedule, loweringOptions);
    if (!lowered.ok()) {
      std::string message = "target lowering failed";
      if (!lowered.diagnostics.empty())
        message += ": " + lowered.diagnostics.front().message;
      return failure(result, CompileDFGStatus::TargetLoweringFailure, std::move(message),
                     artifacts);
    }
    std::string controlVerificationError;
    const auto controlVerification = lowering::TargetControlProgramVerifier::verify(
        targetDFG, target, *allocated.mapping, *materialized.schedule, *lowered.controls,
        &controlVerificationError);
    artifacts.write("18_target_controls.json",
                    Json{{"ii", lowered.controls->ii()},
                         {"trip_count", lowered.controls->tripCount()},
                         {"prologue_cycles", lowered.controls->prologue().cycles.size()},
                         {"kernel_cycles", lowered.controls->kernel().body.size()},
                         {"kernel_repeat_count", lowered.controls->kernel().repeatCount},
                         {"epilogue_cycles", lowered.controls->epilogue().cycles.size()}}
                        .dump(2));
    artifacts.write("19_target_control_verification.json",
                    verification(controlVerification, controlVerificationError).dump(2));
    if (!controlVerification)
      return failure(result, CompileDFGStatus::TargetControlVerificationFailure,
                     controlVerificationError, artifacts);
    artifacts.write("20_program_manifest.json", lowered.manifest->json);
    artifacts.write("21_lowering_report.json",
                    Json{{"ok", true},
                         {"encoded_controls", lowered.stats.encodedControls},
                         {"node_issues", lowered.stats.nodeIssues},
                         {"link_launches", lowered.stats.linkLaunches},
                         {"rf_reads", lowered.stats.rfReads},
                         {"rf_writes", lowered.stats.rfWrites}}
                        .dump(2));
    result.status = CompileDFGStatus::Success;
    result.message = "all compiler stages and independent verifiers passed";
    result.manifest = lowered.manifest;
    artifacts.write("compiler_pipeline_report.json", result.toJson());
    return result;
  } catch (const std::exception& error) {
    result.status = CompileDFGStatus::InternalError;
    result.message = error.what();
    try {
      ArtifactWriter artifacts(options.artifactDirectory, result);
      artifacts.write("compiler_pipeline_report.json", result.toJson());
    } catch (...) {
      result.status = CompileDFGStatus::ArtifactFailure;
    }
    return result;
  }
}

} // namespace cgra::pipeline
