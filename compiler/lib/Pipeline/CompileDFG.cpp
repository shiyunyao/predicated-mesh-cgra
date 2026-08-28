// SPDX-License-Identifier: MIT
#include "cgra/Pipeline/CompileDFG.h"

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
             {"mapping_status", moduloMapping ? "success" : "not_available"},
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
               {"node_candidate_attempts", stats.nodeCandidateAttempts},
               {"route_state_expansions", stats.routeStateExpansions},
               {"completed_modulo_mappings", stats.completedModuloMappings},
               {"post_mapping_rejected", stats.postMappingRejected},
               {"stage_rejected", stats.stageRejected},
               {"rf_rejected", stats.rfRejected},
               {"post_mapping_abort", stats.postMappingAbort},
               {"max_stage", stats.maxStage},
               {"storage_segments", stats.storageSegments},
               {"prologue_cycles", stats.prologueCycles},
               {"kernel_repeats", stats.kernelRepeats},
               {"epilogue_cycles", stats.epilogueCycles}}},
             {"artifacts", Json::array()}};
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

    const auto legalization = target::TargetLegalizer::legalize(dfg, target);
    artifacts.write("02_legalization.json", legalization.toJson());
    if (!legalization.ok())
      return failure(result, CompileDFGStatus::TargetLegalizationFailure, legalization.format(),
                     artifacts);
    const auto& targetDFG = *legalization.dfg;
    artifacts.write("03_target_dfg.json", target::toJson(targetDFG));
    const auto targetReport = target::TargetDFGVerifier::verify(targetDFG, target, &dfg);
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
    if (options.mode == CompileDFGMode::HardwareExecutable) {
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
    result.stats.postMappingAbort = mapped.stats.postMappingAbort;
    if (!mapped.ok())
      return failure(result, CompileDFGStatus::MappingFailure, mapped.format(), artifacts);
    result.stats.mappedII = mapped.mapping->ii();
    result.moduloMapping = mapped.mapping;
    artifacts.write("07_modulo_mapping.json", mapping::toJson(*mapped.mapping));
    const auto mappingReport =
        mapping::ModuloMappingVerifier::verify(targetDFG, target, *mapped.mapping);
    artifacts.write("08_modulo_mapping_verification.json", mappingReport.toJson());
    if (!mappingReport.ok())
      return failure(result, CompileDFGStatus::ModuloMappingVerificationFailure,
                     mappingReport.format(), artifacts);

    if (options.mode == CompileDFGMode::MappingResearch) {
      const auto physical = completionChecker.check(targetDFG, target, *mapped.mapping);
      result.physicalRealizability.reasonCode = physical.reasonCode;
      result.physicalRealizability.message = physical.message;
      switch (physical.decision) {
      case mapping::CompleteMappingDecision::Accept:
        result.physicalRealizability.status = PhysicalRealizabilityStatus::Feasible;
        break;
      case mapping::CompleteMappingDecision::Reject:
        result.physicalRealizability.status = PhysicalRealizabilityStatus::Infeasible;
        break;
      case mapping::CompleteMappingDecision::Abort:
        result.physicalRealizability.status = PhysicalRealizabilityStatus::Error;
        break;
      }
      result.status = CompileDFGStatus::Success;
      result.message = "verified modulo mapping produced; physical realizability recorded separately";
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
    artifacts.write("13_rf_allocated_mapping.json",
                    register_allocation::toJson(*allocated.mapping));
    const auto rfReport =
        register_allocation::RFAllocationVerifier::verify(targetDFG, target, *allocated.mapping);
    artifacts.write("14_rf_verification.json",
                    verification(rfReport.ok(), rfReport.format()).dump(2));
    if (!rfReport.ok())
      return failure(result, CompileDFGStatus::RFVerificationFailure, rfReport.format(), artifacts);

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
