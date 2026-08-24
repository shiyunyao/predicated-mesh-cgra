// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Lowering/ConstantAllocator.h"
#include "cgra/Lowering/TargetControlProgram.h"
#include "cgra/Target/ControlLayout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cgra::lowering {

struct EncodedTargetCycle {
  std::vector<EncodedControl> tiles;
};
struct EncodedTargetPhase {
  std::vector<EncodedTargetCycle> cycles;
};
struct EncodedRepeatingKernel {
  std::vector<EncodedTargetCycle> body;
  std::uint64_t repeatCount = 0;
};
struct EncodedTargetProgram {
  EncodedTargetPhase prologue;
  EncodedRepeatingKernel kernel;
  EncodedTargetPhase epilogue;
};

struct ProgramManifest {
  std::string json;
};

enum class TargetLoweringStatus {
  Success,
  InvalidTargetDFG,
  InvalidRFAllocatedMapping,
  InvalidMaterializedSchedule,
  UnsupportedOperationLowering,
  UnsupportedValueSource,
  UnsupportedBoundaryProvider,
  UnsupportedExternalProvider,
  ConstantCapacityExceeded,
  ControlFieldConflict,
  SourceConnectivityViolation,
  RFAccessPortViolation,
  ControlEncodingFailure,
  ManifestBuildFailure,
  ManifestValidationFailure,
  VerificationFailure,
  ArithmeticOverflow,
  InternalError,
};

struct TargetLoweringDiagnostic {
  TargetLoweringStatus status = TargetLoweringStatus::InternalError;
  std::string message;
  std::optional<std::uint32_t> node;
  std::optional<std::uint32_t> edge;
  std::optional<std::uint32_t> cycle;
  std::optional<std::uint32_t> row;
  std::optional<std::uint32_t> col;
};

struct TargetLoweringStats {
  std::uint64_t semanticCycles = 0;
  std::uint64_t activeTileCycles = 0;
  std::uint64_t nodeIssues = 0;
  std::uint64_t linkLaunches = 0;
  std::uint64_t rfReads = 0;
  std::uint64_t rfWrites = 0;
  std::uint64_t encodedControls = 0;
  std::uint64_t idempotentMerges = 0;
};

struct TargetLoweringOptions {
  std::string programName = "cgra-program";
  std::string targetPath;
  std::string observation = "semantic schedule";
  std::vector<std::pair<std::uint32_t, std::uint32_t>> scratchpadPreload;
  ConstantImage constantImage;
};

struct TargetLoweringResult {
  TargetLoweringStatus status = TargetLoweringStatus::InternalError;
  std::optional<TargetControlProgram> controls;
  std::optional<EncodedTargetProgram> encoded;
  std::optional<ProgramManifest> manifest;
  TargetLoweringStats stats;
  std::vector<TargetLoweringDiagnostic> diagnostics;
  ConstantImage constantImage;

  bool ok() const noexcept { return status == TargetLoweringStatus::Success; }
};

} // namespace cgra::lowering
