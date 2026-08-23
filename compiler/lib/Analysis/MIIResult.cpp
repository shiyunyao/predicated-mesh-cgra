// SPDX-License-Identifier: MIT
#include "cgra/Analysis/MIIResult.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace cgra::analysis {
namespace {

using Json = nlohmann::json;

} // namespace

std::string_view toString(MIIStatus status) noexcept {
  switch (status) {
  case MIIStatus::Success:
    return "success";
  case MIIStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case MIIStatus::NoCompatibleResource:
    return "no_compatible_resource";
  case MIIStatus::UnschedulableZeroDistanceCycle:
    return "unschedulable_zero_distance_cycle";
  case MIIStatus::TargetContractError:
    return "target_contract_error";
  case MIIStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(MIIAnalysisDiagnosticCode code) noexcept {
#define CGRA_MII_CODE(name)                                                                        \
  case MIIAnalysisDiagnosticCode::name:                                                            \
    return #name
  switch (code) {
    CGRA_MII_CODE(MII_INVALID_TARGET_DFG);
    CGRA_MII_CODE(MII_NO_COMPATIBLE_RESOURCE);
    CGRA_MII_CODE(MII_ZERO_DISTANCE_CYCLE);
    CGRA_MII_CODE(MII_TARGET_LATENCY_MISSING);
    CGRA_MII_CODE(MII_TARGET_OCCUPANCY_INVALID);
    CGRA_MII_CODE(MII_TARGET_MEMORY_TIMING_MISSING);
    CGRA_MII_CODE(MII_ARITHMETIC_OVERFLOW);
    CGRA_MII_CODE(MII_INTERNAL_ERROR);
  }
#undef CGRA_MII_CODE
  return "MII_INTERNAL_ERROR";
}

std::string MIIResult::format() const {
  std::ostringstream output;
  output << "MII status: " << toString(status) << '\n';
  output << "Resource MII: " << resourceMII << '\n';
  output << "  self occupancy: " << resourceBreakdown.selfOccupancyMII << '\n';
  output << "  FU pressure: " << resourceBreakdown.fuMII << '\n';
  output << "  LSU pressure: " << resourceBreakdown.lsuMII << '\n';
  output << "  per-operation: " << resourceBreakdown.perOperationMII << '\n';
  output << "Recurrence MII: " << recurrenceMII << '\n';
  output << "MII: " << mii;
  for (const auto& diagnostic : diagnostics) {
    output << '\n' << toString(diagnostic.code);
    if (diagnostic.node)
      output << " node=%n" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=%e" << *diagnostic.edge;
    output << ": " << diagnostic.message;
  }
  return output.str();
}

std::string MIIResult::toJson() const {
  Json root = {{"schema", "cgra.mii.analysis.v1"},
               {"status", toString(status)},
               {"resource_mii", resourceMII},
               {"recurrence_mii", recurrenceMII},
               {"mii", mii},
               {"resource_breakdown",
                {{"self_occupancy", resourceBreakdown.selfOccupancyMII},
                 {"fu", resourceBreakdown.fuMII},
                 {"lsu", resourceBreakdown.lsuMII},
                 {"per_operation", resourceBreakdown.perOperationMII}}},
               {"diagnostics", Json::array()}};
  if (recurrenceWitness) {
    root["recurrence_witness"] = {{"edges", recurrenceWitness->edges},
                                  {"total_separation", recurrenceWitness->totalSeparation},
                                  {"total_distance", recurrenceWitness->totalDistance}};
  }
  for (const auto& diagnostic : diagnostics) {
    Json value = {{"code", toString(diagnostic.code)}, {"message", diagnostic.message}};
    if (diagnostic.node)
      value["node"] = *diagnostic.node;
    if (diagnostic.edge)
      value["edge"] = *diagnostic.edge;
    root["diagnostics"].push_back(std::move(value));
  }
  return root.dump(2) + '\n';
}

} // namespace cgra::analysis
