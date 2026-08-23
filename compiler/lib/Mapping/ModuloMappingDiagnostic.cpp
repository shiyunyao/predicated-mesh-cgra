// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingDiagnostic.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace cgra::mapping {
namespace {
using Json = nlohmann::json;
}

std::string_view toString(MappingDiagnosticSeverity severity) noexcept {
  return severity == MappingDiagnosticSeverity::Error ? "error" : "warning";
}

std::string_view toString(MappingDiagnosticCode code) noexcept {
#define MMAP_CODE(name)                                                                            \
  case MappingDiagnosticCode::name:                                                                \
    return #name
  switch (code) {
    MMAP_CODE(MMAP_INVALID_TARGET_DFG);
    MMAP_CODE(MMAP_INVALID_II);
    MMAP_CODE(MMAP_TARGET_NAME_MISMATCH);
    MMAP_CODE(MMAP_UNKNOWN_NODE);
    MMAP_CODE(MMAP_UNKNOWN_EDGE);
    MMAP_CODE(MMAP_NODE_MISSING_PLACEMENT);
    MMAP_CODE(MMAP_NODE_DUPLICATE_PLACEMENT);
    MMAP_CODE(MMAP_EDGE_MISSING_REALIZATION);
    MMAP_CODE(MMAP_EDGE_DUPLICATE_REALIZATION);
    MMAP_CODE(MMAP_TILE_OUT_OF_RANGE);
    MMAP_CODE(MMAP_SLOT_OUT_OF_RANGE);
    MMAP_CODE(MMAP_OPERATION_UNSUPPORTED_ON_TILE);
    MMAP_CODE(MMAP_EXECUTION_RESOURCE_MISSING);
    MMAP_CODE(MMAP_FU_RESOURCE_CONFLICT);
    MMAP_CODE(MMAP_LSU_RESOURCE_CONFLICT);
    MMAP_CODE(MMAP_DATA_LINK_CONFLICT);
    MMAP_CODE(MMAP_PRED_LINK_CONFLICT);
    MMAP_CODE(MMAP_OPERATION_SELF_OVERLAP);
    MMAP_CODE(MMAP_ROUTE_SELF_RESOURCE_CONFLICT);
    MMAP_CODE(MMAP_TRANSPORT_MISSING_FOR_VALUE_EDGE);
    MMAP_CODE(MMAP_TRANSPORT_UNEXPECTED_FOR_MEMORY_EDGE);
    MMAP_CODE(MMAP_TRANSPORT_DOMAIN_MISMATCH);
    MMAP_CODE(MMAP_TRANSPORT_BAD_START);
    MMAP_CODE(MMAP_TRANSPORT_BAD_END);
    MMAP_CODE(MMAP_TRANSPORT_DISCONTINUITY);
    MMAP_CODE(MMAP_LINK_INVALID_TOPOLOGY);
    MMAP_CODE(MMAP_LINK_TIME_BEFORE_VALUE_READY);
    MMAP_CODE(MMAP_LINK_TIME_REGRESSION);
    MMAP_CODE(MMAP_LINK_TIME_GAP_WITHOUT_STORAGE);
    MMAP_CODE(MMAP_HOLD_INVALID_INTERVAL);
    MMAP_CODE(MMAP_HOLD_WRONG_TILE);
    MMAP_CODE(MMAP_HOLD_BEFORE_VALUE_READY);
    MMAP_CODE(MMAP_HOLD_DISCONTINUITY);
    MMAP_CODE(MMAP_REQUIRED_SEPARATION_MISMATCH);
    MMAP_CODE(MMAP_MEMORY_TRANSPORT_PRESENT);
    MMAP_CODE(MMAP_MEMORY_SEPARATION_TOO_SMALL);
    MMAP_CODE(MMAP_MEMORY_SEPARATION_MISMATCH);
    MMAP_CODE(MMAP_TARGET_TIMING_MISSING);
    MMAP_CODE(MMAP_INTERNAL_ERROR);
  }
#undef MMAP_CODE
  return "MMAP_INTERNAL_ERROR";
}

std::size_t ModuloMappingVerificationReport::errorCount() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(diagnostics_.begin(), diagnostics_.end(), [](const auto& diagnostic) {
        return diagnostic.severity == MappingDiagnosticSeverity::Error;
      }));
}

std::size_t ModuloMappingVerificationReport::warningCount() const noexcept {
  return diagnostics_.size() - errorCount();
}

bool ModuloMappingVerificationReport::contains(MappingDiagnosticCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string ModuloMappingVerificationReport::format() const {
  std::ostringstream output;
  output << (ok() ? "valid" : "invalid") << " modulo mapping";
  for (const auto& diagnostic : diagnostics_) {
    output << '\n' << toString(diagnostic.severity) << " [" << toString(diagnostic.code) << "]";
    if (diagnostic.node)
      output << " node=%n" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=%e" << *diagnostic.edge;
    if (diagnostic.tile)
      output << " tile=" << toString(*diagnostic.tile);
    if (diagnostic.slot)
      output << " slot=" << *diagnostic.slot;
    if (diagnostic.actionIndex)
      output << " action=" << *diagnostic.actionIndex;
    output << ": " << diagnostic.message;
  }
  return output.str();
}

std::string ModuloMappingVerificationReport::toJson() const {
  Json root = {{"schema", "cgra.modulo_mapping.verification.v1"},
               {"valid", ok()},
               {"errors", errorCount()},
               {"warnings", warningCount()},
               {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics_) {
    Json value = {{"severity", toString(diagnostic.severity)},
                  {"code", toString(diagnostic.code)},
                  {"message", diagnostic.message}};
    if (diagnostic.node)
      value["node"] = *diagnostic.node;
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    if (diagnostic.resource)
      value["resource"] = *diagnostic.resource;
    if (diagnostic.tile)
      value["tile"] = Json::array({diagnostic.tile->row, diagnostic.tile->col});
    if (diagnostic.slot)
      value["slot"] = *diagnostic.slot;
    if (diagnostic.actionIndex)
      value["action_index"] = *diagnostic.actionIndex;
    root["diagnostics"].push_back(std::move(value));
  }
  return root.dump(2) + '\n';
}

} // namespace cgra::mapping
