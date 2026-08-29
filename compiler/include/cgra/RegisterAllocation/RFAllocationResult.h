// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"
#include "cgra/RegisterAllocation/RFAllocationBudget.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::register_allocation {

enum class RFAllocationStatus {
  Success,
  InvalidTargetDFG,
  InvalidStagedMapping,
  TargetRFContractError,
  FixedRegisterSelfOverlap,
  ReadPortConflict,
  WritePortConflict,
  SameAddressRWConflict,
  RegisterDepthInfeasible,
  RotationFactorOverflow,
  RotationPeriodExceedsControlMemory,
  BudgetExceeded,
  VerificationFailure,
  ArithmeticOverflow,
  InternalError,
};

enum class RFAllocationDiagnosticCode : std::uint32_t {
  RFA_INVALID_TARGET_DFG,
  RFA_INVALID_STAGED_MAPPING,
  RFA_TARGET_BANK_MISSING,
  RFA_TARGET_RF_CONTRACT_INVALID,
  RFA_STORAGE_TIMING_INVALID,
  RFA_STORAGE_ARITHMETIC_OVERFLOW,
  RFA_FIXED_REGISTER_SELF_OVERLAP,
  RFA_READ_PORT_CONFLICT,
  RFA_WRITE_PORT_CONFLICT,
  RFA_SAME_ADDRESS_RW_CONFLICT,
  RFA_REGISTER_DEPTH_INFEASIBLE,
  RFA_ROTATION_FACTOR_OVERFLOW,
  RFA_ROTATION_PERIOD_EXCEEDS_CONTROL_MEMORY,
  RFA_COLORING_BUDGET_EXCEEDED,
  RFA_UNKNOWN_STORAGE_SEGMENT,
  RFA_DUPLICATE_STORAGE_ALLOCATION,
  RFA_UNALLOCATED_STORAGE_SEGMENT,
  RFA_INVALID_REGISTER_INDEX,
  RFA_BANK_DOMAIN_MISMATCH,
  RFA_FINAL_VERIFICATION_FAILED,
  RFA_INTERNAL_ERROR,
};

struct RFAllocationDiagnostic {
  RFAllocationDiagnosticCode code = RFAllocationDiagnosticCode::RFA_INTERNAL_ERROR;
  std::string message;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<StorageSegmentId> segment;
  std::optional<cgra::mapping::TileCoord> tile;
  std::optional<cgra::RegisterBankId> bank;
  std::optional<std::uint32_t> reg;
  std::optional<StorageSegmentId> conflictingSegment;
};

struct RFAllocationStats {
  std::uint64_t storageSegments = 0;
  std::uint64_t dataSegments = 0;
  std::uint64_t predicateSegments = 0;
  std::uint64_t explicitHoldSegments = 0;
  std::uint64_t terminalSlackSegments = 0;
  std::uint64_t coalescedOrigins = 0;
  std::uint64_t conflictEdges = 0;
  std::uint64_t coloringDecisions = 0;
  std::uint64_t coloringBacktracks = 0;
  std::uint32_t maxRegistersUsedOnAnyBank = 0;
  std::uint32_t maxReadPortsUsed = 0;
  std::uint32_t maxWritePortsUsed = 0;
};

struct RFAllocationResult {
  RFAllocationStatus status = RFAllocationStatus::InternalError;
  std::optional<RFAllocatedMapping> mapping;
  RFAllocationStats stats;
  std::vector<RFAllocationDiagnostic> diagnostics;

  bool ok() const noexcept { return status == RFAllocationStatus::Success && mapping.has_value(); }
  std::string format() const;
  std::string toJson() const;
};

std::string_view toString(RFAllocationStatus status) noexcept;
std::string_view toString(RFAllocationDiagnosticCode code) noexcept;

} // namespace cgra::register_allocation
