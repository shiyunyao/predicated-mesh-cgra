// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/TargetDFG.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::analysis {

enum class MIIStatus {
  Success,
  InvalidTargetDFG,
  NoCompatibleResource,
  UnschedulableZeroDistanceCycle,
  TargetContractError,
  InternalError,
};

enum class MIIAnalysisDiagnosticCode : std::uint32_t {
  MII_INVALID_TARGET_DFG,
  MII_NO_COMPATIBLE_RESOURCE,
  MII_ZERO_DISTANCE_CYCLE,
  MII_TARGET_LATENCY_MISSING,
  MII_TARGET_OCCUPANCY_INVALID,
  MII_TARGET_MEMORY_TIMING_MISSING,
  MII_ARITHMETIC_OVERFLOW,
  MII_INTERNAL_ERROR,
};

std::string_view toString(MIIStatus status) noexcept;
std::string_view toString(MIIAnalysisDiagnosticCode code) noexcept;

struct MIIAnalysisDiagnostic {
  MIIAnalysisDiagnosticCode code = MIIAnalysisDiagnosticCode::MII_INTERNAL_ERROR;
  std::string message;
  std::optional<target::TargetNodeId> node;
  std::optional<target::TargetEdgeId> edge;
};

struct ResourceMIIBreakdown {
  std::uint32_t selfOccupancyMII = 1;
  std::uint32_t fuMII = 1;
  std::uint32_t lsuMII = 1;
  std::uint32_t perOperationMII = 1;

  friend bool operator==(const ResourceMIIBreakdown&, const ResourceMIIBreakdown&) = default;
};

struct RecurrenceWitness {
  std::vector<target::TargetEdgeId> edges;
  std::uint64_t totalSeparation = 0;
  std::uint64_t totalDistance = 0;

  friend bool operator==(const RecurrenceWitness&, const RecurrenceWitness&) = default;
};

struct MIIResult {
  MIIStatus status = MIIStatus::InternalError;
  std::uint32_t resourceMII = 0;
  std::uint32_t recurrenceMII = 0;
  std::uint32_t mii = 0;
  ResourceMIIBreakdown resourceBreakdown;
  std::optional<RecurrenceWitness> recurrenceWitness;
  std::vector<MIIAnalysisDiagnostic> diagnostics;

  bool ok() const noexcept { return status == MIIStatus::Success; }
  std::string format() const;
  std::string toJson() const;
};

} // namespace cgra::analysis
