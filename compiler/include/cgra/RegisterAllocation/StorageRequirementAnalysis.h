// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/StorageRequirement.h"
#include "cgra/Schedule/StagedMapping.h"
#include "cgra/Target/TargetDFG.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::register_allocation {

enum class StorageRequirementStatus {
  Success,
  InvalidTargetDFG,
  InvalidStagedMapping,
  TargetRFContractError,
  ArithmeticOverflow,
  InternalError,
};

enum class StorageRequirementDiagnosticCode : std::uint32_t {
  RFA_INVALID_TARGET_DFG,
  RFA_INVALID_STAGED_MAPPING,
  RFA_TARGET_BANK_MISSING,
  RFA_TARGET_RF_CONTRACT_INVALID,
  RFA_STORAGE_TIMING_INVALID,
  RFA_STORAGE_ARITHMETIC_OVERFLOW,
  RFA_INTERNAL_ERROR,
};

struct StorageRequirementDiagnostic {
  StorageRequirementDiagnosticCode code = StorageRequirementDiagnosticCode::RFA_INTERNAL_ERROR;
  std::string message;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<StorageSegmentId> segment;
};

struct StorageRequirementStats {
  std::uint64_t storageSegments = 0;
  std::uint64_t dataSegments = 0;
  std::uint64_t predicateSegments = 0;
  std::uint64_t explicitHoldSegments = 0;
  std::uint64_t terminalSlackSegments = 0;
  std::uint64_t coalescedOrigins = 0;
};

struct StorageRequirementResult {
  StorageRequirementStatus status = StorageRequirementStatus::InternalError;
  std::optional<StorageRequirements> requirements;
  StorageRequirementStats stats;
  std::vector<StorageRequirementDiagnostic> diagnostics;

  bool ok() const noexcept {
    return status == StorageRequirementStatus::Success && requirements.has_value();
  }
  std::string format() const;
};

std::string_view toString(StorageRequirementStatus status) noexcept;
std::string_view toString(StorageRequirementDiagnosticCode code) noexcept;

class StorageRequirementAnalysis {
public:
  static StorageRequirementResult analyze(const cgra::target::TargetDFG& dfg,
                                          const cgra::TargetModel& target,
                                          const cgra::schedule::StagedMapping& mapping);
};

} // namespace cgra::register_allocation
