// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"
#include "cgra/Lowering/TargetLoweringResult.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/RegisterAllocation/RFAllocationBudget.h"
#include "cgra/Schedule/ScheduleMaterializationResult.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cgra::pipeline {

enum class CompileDFGMode {
  HardwareExecutable,
  MappingResearch,
};

std::string_view toString(CompileDFGMode mode) noexcept;

enum class PhysicalRealizabilityStatus {
  NotRun,
  Feasible,
  Infeasible,
  Error,
};

std::string_view toString(PhysicalRealizabilityStatus status) noexcept;

struct PhysicalRealizabilityResult {
  PhysicalRealizabilityStatus status = PhysicalRealizabilityStatus::NotRun;
  std::string reasonCode;
  std::string message;
};

enum class CompileDFGStatus {
  Success,
  GenericDFGVerificationFailure,
  TargetLegalizationFailure,
  TargetDFGVerificationFailure,
  MIIAnalysisFailure,
  MappingFailure,
  ModuloMappingVerificationFailure,
  StageSchedulingFailure,
  StageVerificationFailure,
  RFAllocationFailure,
  RFVerificationFailure,
  MaterializationFailure,
  MaterializedScheduleVerificationFailure,
  TargetLoweringFailure,
  TargetControlVerificationFailure,
  ManifestValidationFailure,
  ArtifactFailure,
  InternalError,
};

std::string_view toString(CompileDFGStatus status) noexcept;

struct CompileDFGOptions {
  CompileDFGMode mode = CompileDFGMode::HardwareExecutable;
  std::uint64_t tripCount = 1;
  std::filesystem::path targetPath;
  std::filesystem::path artifactDirectory;
  mapping::ModuloMapperOptions mapper;
  register_allocation::RFAllocationOptions rfAllocation;
  schedule::ScheduleMaterializationBudget materializationBudget;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> scratchpadPreload;
  std::string programName = "compiled-dfg";
};

struct CompileDFGStats {
  std::uint64_t tripCount = 0;
  std::uint32_t mii = 0;
  bool mapperInvoked = false;
  std::uint32_t mappedII = 0;
  std::uint64_t nodeCandidateAttempts = 0;
  std::uint64_t routeStateExpansions = 0;
  std::uint64_t completedModuloMappings = 0;
  std::uint64_t postMappingRejected = 0;
  std::uint64_t stageRejected = 0;
  std::uint64_t rfRejected = 0;
  std::uint64_t postMappingAbort = 0;
  std::uint64_t maxStage = 0;
  std::uint64_t storageSegments = 0;
  std::uint64_t prologueCycles = 0;
  std::uint64_t kernelRepeats = 0;
  std::uint64_t epilogueCycles = 0;
};

struct CompileDFGResult {
  CompileDFGStatus status = CompileDFGStatus::InternalError;
  CompileDFGMode mode = CompileDFGMode::HardwareExecutable;
  std::string message;
  std::optional<mapping::ModuloMapping> moduloMapping;
  PhysicalRealizabilityResult physicalRealizability;
  std::optional<lowering::ProgramManifest> manifest;
  CompileDFGStats stats;
  std::vector<std::filesystem::path> artifacts;

  bool ok() const noexcept {
    if (status != CompileDFGStatus::Success || !moduloMapping)
      return false;
    return mode == CompileDFGMode::MappingResearch || manifest.has_value();
  }
  bool hardwareExecutable() const noexcept { return manifest.has_value(); }
  std::string toJson() const;
};

CompileDFGResult compileGenericDFG(const ir::DFG& dfg, const TargetModel& target,
                                   const CompileDFGOptions& options);

} // namespace cgra::pipeline
