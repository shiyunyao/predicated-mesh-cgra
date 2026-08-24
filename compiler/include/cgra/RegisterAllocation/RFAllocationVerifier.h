// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"
#include "cgra/Target/TargetDFG.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cgra::register_allocation {

enum class RFAllocationVerificationCode : std::uint32_t {
  RFA_INVALID_TARGET_DFG,
  RFA_INVALID_STAGED_MAPPING,
  RFA_UNKNOWN_STORAGE_SEGMENT,
  RFA_DUPLICATE_STORAGE_ALLOCATION,
  RFA_UNALLOCATED_STORAGE_SEGMENT,
  RFA_INVALID_REGISTER_INDEX,
  RFA_BANK_DOMAIN_MISMATCH,
  RFA_FIXED_REGISTER_SELF_OVERLAP,
  RFA_READ_PORT_CONFLICT,
  RFA_WRITE_PORT_CONFLICT,
  RFA_SAME_ADDRESS_RW_CONFLICT,
  RFA_PERIODIC_REGISTER_CONFLICT,
  RFA_STORAGE_TIMING_INVALID,
};

struct RFAllocationVerificationDiagnostic {
  RFAllocationVerificationCode code = RFAllocationVerificationCode::RFA_STORAGE_TIMING_INVALID;
  std::string message;
  std::optional<StorageSegmentId> segment;
  std::optional<StorageSegmentId> conflictingSegment;
};

class RFAllocationVerificationReport {
public:
  bool ok() const noexcept { return diagnostics_.empty(); }
  bool contains(RFAllocationVerificationCode code) const noexcept;
  std::span<const RFAllocationVerificationDiagnostic> diagnostics() const noexcept {
    return diagnostics_;
  }
  std::string format() const;
  void add(RFAllocationVerificationDiagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
  }

private:
  friend class RFAllocationVerifier;
  std::vector<RFAllocationVerificationDiagnostic> diagnostics_;
};

class RFAllocationVerifier {
public:
  static RFAllocationVerificationReport verify(const cgra::target::TargetDFG& dfg,
                                               const cgra::TargetModel& target,
                                               const RFAllocatedMapping& mapping);
};

} // namespace cgra::register_allocation
