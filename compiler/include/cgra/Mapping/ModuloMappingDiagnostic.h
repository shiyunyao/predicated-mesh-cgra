// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloResource.h"
#include "cgra/Target/TargetDFG.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cgra::mapping {

enum class MappingDiagnosticSeverity {
  Error,
  Warning,
};

enum class MappingDiagnosticCode : std::uint32_t {
  MMAP_INVALID_TARGET_DFG,
  MMAP_INVALID_II,
  MMAP_TARGET_NAME_MISMATCH,
  MMAP_UNKNOWN_NODE,
  MMAP_UNKNOWN_EDGE,
  MMAP_NODE_MISSING_PLACEMENT,
  MMAP_NODE_DUPLICATE_PLACEMENT,
  MMAP_EDGE_MISSING_REALIZATION,
  MMAP_EDGE_DUPLICATE_REALIZATION,
  MMAP_TILE_OUT_OF_RANGE,
  MMAP_SLOT_OUT_OF_RANGE,
  MMAP_OPERATION_UNSUPPORTED_ON_TILE,
  MMAP_EXECUTION_RESOURCE_MISSING,
  MMAP_FU_RESOURCE_CONFLICT,
  MMAP_LSU_RESOURCE_CONFLICT,
  MMAP_DATA_LINK_CONFLICT,
  MMAP_PRED_LINK_CONFLICT,
  MMAP_OPERATION_SELF_OVERLAP,
  MMAP_ROUTE_SELF_RESOURCE_CONFLICT,
  MMAP_TRANSPORT_MISSING_FOR_VALUE_EDGE,
  MMAP_TRANSPORT_UNEXPECTED_FOR_MEMORY_EDGE,
  MMAP_TRANSPORT_DOMAIN_MISMATCH,
  MMAP_TRANSPORT_BAD_START,
  MMAP_TRANSPORT_BAD_END,
  MMAP_TRANSPORT_DISCONTINUITY,
  MMAP_LINK_INVALID_TOPOLOGY,
  MMAP_LINK_TIME_BEFORE_VALUE_READY,
  MMAP_LINK_TIME_REGRESSION,
  MMAP_LINK_TIME_GAP_WITHOUT_STORAGE,
  MMAP_HOLD_INVALID_INTERVAL,
  MMAP_HOLD_WRONG_TILE,
  MMAP_HOLD_BEFORE_VALUE_READY,
  MMAP_HOLD_DISCONTINUITY,
  MMAP_REQUIRED_SEPARATION_MISMATCH,
  MMAP_MEMORY_TRANSPORT_PRESENT,
  MMAP_MEMORY_SEPARATION_TOO_SMALL,
  MMAP_MEMORY_SEPARATION_MISMATCH,
  MMAP_TARGET_TIMING_MISSING,
  MMAP_INTERNAL_ERROR,
};

struct MappingDiagnostic {
  MappingDiagnosticSeverity severity = MappingDiagnosticSeverity::Error;
  MappingDiagnosticCode code = MappingDiagnosticCode::MMAP_INTERNAL_ERROR;
  std::string message;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
  std::optional<ResourceId> resource;
  std::optional<TileCoord> tile;
  std::optional<std::uint32_t> slot;
  std::optional<std::uint32_t> actionIndex;
};

std::string_view toString(MappingDiagnosticSeverity severity) noexcept;
std::string_view toString(MappingDiagnosticCode code) noexcept;

class ModuloMappingVerificationReport {
public:
  bool ok() const noexcept { return errorCount() == 0; }
  std::size_t errorCount() const noexcept;
  std::size_t warningCount() const noexcept;
  bool contains(MappingDiagnosticCode code) const noexcept;
  std::span<const MappingDiagnostic> diagnostics() const noexcept { return diagnostics_; }
  std::string format() const;
  std::string toJson() const;
  void add(MappingDiagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

private:
  std::vector<MappingDiagnostic> diagnostics_;
};

} // namespace cgra::mapping
