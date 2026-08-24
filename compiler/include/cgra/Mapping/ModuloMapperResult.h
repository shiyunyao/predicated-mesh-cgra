// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapperStats.h"
#include "cgra/Mapping/ModuloMapping.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::mapping {

enum class ModuloMapperStatus {
  Success,
  InvalidTargetDFG,
  MIIAnalysisFailure,
  NoMappingWithinIILimit,
  BudgetExceeded,
  RouteBudgetExceeded,
  VerificationFailure,
  TargetContractError,
  InternalError,
};

enum class ModuloMapperDiagnosticCode {
  MAP_INVALID_TARGET_DFG,
  MAP_MII_ANALYSIS_FAILED,
  MAP_NO_COMPATIBLE_CANDIDATE,
  MAP_NODE_RESOURCE_CONFLICT,
  MAP_ROUTE_NO_PATH,
  MAP_ROUTE_BUDGET_EXCEEDED,
  MAP_GLOBAL_BUDGET_EXCEEDED,
  MAP_NO_MAPPING_WITHIN_II_LIMIT,
  MAP_FINAL_VERIFICATION_FAILED,
  MAP_TARGET_CONTRACT_ERROR,
  MAP_INTERNAL_ERROR,
};

struct ModuloMapperDiagnostic {
  ModuloMapperDiagnosticCode code = ModuloMapperDiagnosticCode::MAP_INTERNAL_ERROR;
  std::string message;
  std::optional<std::uint32_t> ii;
  std::optional<cgra::target::TargetNodeId> node;
  std::optional<cgra::target::TargetEdgeId> edge;
};

std::string_view toString(ModuloMapperStatus status) noexcept;
std::string_view toString(ModuloMapperDiagnosticCode code) noexcept;

struct ModuloMapperResult {
  ModuloMapperStatus status = ModuloMapperStatus::InternalError;
  std::optional<ModuloMapping> mapping;
  ModuloMapperStats stats;
  std::vector<ModuloMapperDiagnostic> diagnostics;

  bool ok() const noexcept { return status == ModuloMapperStatus::Success && mapping.has_value(); }
  std::string format() const;
  std::string toJson() const;
};

} // namespace cgra::mapping
