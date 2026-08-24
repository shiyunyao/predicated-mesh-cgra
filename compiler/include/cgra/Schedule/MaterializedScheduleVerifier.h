// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"
#include "cgra/Schedule/MaterializedSchedule.h"
#include "cgra/Schedule/ScheduleMaterializationResult.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cgra::schedule {

enum class MaterializedScheduleVerificationCode : std::uint32_t {
  MAT_INVALID_TARGET_DFG,
  MAT_INVALID_RF_MAPPING,
  MAT_INVALID_TRIP_COUNT,
  MAT_UNKNOWN_EVENT,
  MAT_DUPLICATE_EVENT,
  MAT_MISSING_EVENT,
  MAT_WRONG_EVENT_TIME,
  MAT_WRONG_EVENT_PROVENANCE,
  MAT_MEMORY_TRANSPORT_EVENT,
  MAT_KERNEL_SHAPE_INVALID,
  MAT_OUTPUT_OVERFLOW,
};

struct MaterializedScheduleVerificationDiagnostic {
  MaterializedScheduleVerificationCode code;
  std::string message;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<cgra::register_allocation::StorageSegmentId> segment;
};

class MaterializedScheduleVerificationReport {
public:
  bool ok() const noexcept { return diagnostics_.empty(); }
  bool contains(MaterializedScheduleVerificationCode code) const noexcept;
  std::span<const MaterializedScheduleVerificationDiagnostic> diagnostics() const noexcept {
    return diagnostics_;
  }
  std::string format() const;
  void add(MaterializedScheduleVerificationDiagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
  }

private:
  std::vector<MaterializedScheduleVerificationDiagnostic> diagnostics_;
};

class MaterializedScheduleVerifier {
public:
  static MaterializedScheduleVerificationReport
  verify(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
         const cgra::register_allocation::RFAllocatedMapping& mapping,
         const ScheduleMaterializationRequest& request, const MaterializedSchedule& schedule);
};

} // namespace cgra::schedule
