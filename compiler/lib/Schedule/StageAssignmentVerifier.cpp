// SPDX-License-Identifier: MIT
#include "cgra/Schedule/StageAssignmentVerifier.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <sstream>

namespace cgra::schedule {
namespace {

using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

void add(StageAssignmentVerificationReport& report, StageAssignmentDiagnosticCode code,
         std::string message, std::optional<NodeId> node = std::nullopt,
         std::optional<EdgeId> edge = std::nullopt) {
  report.add({code, std::move(message), node, edge});
}

bool checkedMultiplyAdd(std::uint64_t lhs, std::uint64_t multiplier, std::uint64_t addend,
                        std::uint64_t& result) {
  if (multiplier != 0 && lhs > (std::numeric_limits<std::uint64_t>::max() - addend) / multiplier)
    return false;
  result = lhs * multiplier + addend;
  return true;
}

} // namespace

bool StageAssignmentVerificationReport::contains(
    StageAssignmentDiagnosticCode code) const noexcept {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                     [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::string StageAssignmentVerificationReport::format() const {
  std::ostringstream output;
  for (const auto& diagnostic : diagnostics_) {
    output << "[stage] " << diagnostic.message;
    if (diagnostic.node)
      output << " node=" << *diagnostic.node;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    output << '\n';
  }
  return output.str();
}

StageAssignmentVerificationReport
StageAssignmentVerifier::verify(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                                const StagedMapping& mapping) {
  StageAssignmentVerificationReport report;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    add(report, StageAssignmentDiagnosticCode::STAGE_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return report;
  }
  if (dfg.targetName() != target.name()) {
    add(report, StageAssignmentDiagnosticCode::STAGE_INVALID_TARGET_DFG,
        "TargetDFG target name does not match selected TargetModel");
    return report;
  }
  const auto moduloReport =
      cgra::mapping::ModuloMappingVerifier::verify(dfg, target, mapping.modulo());
  if (!moduloReport.ok()) {
    add(report, StageAssignmentDiagnosticCode::STAGE_INVALID_MODULO_MAPPING,
        "ModuloMapping precondition failed: " + moduloReport.format());
    return report;
  }
  if (mapping.stages().size() != dfg.nodes().size()) {
    add(report, StageAssignmentDiagnosticCode::STAGE_MISSING_STAGE,
        "staged mapping must contain exactly one stage per TargetDFG node");
  }
  for (const auto& entry : mapping.stages()) {
    if (!dfg.containsNode(entry.node))
      add(report, StageAssignmentDiagnosticCode::STAGE_UNKNOWN_NODE,
          "staged mapping contains an unknown target node", entry.node);
  }
  for (const auto& node : dfg.nodes()) {
    const auto found = std::find_if(mapping.stages().begin(), mapping.stages().end(),
                                    [&node](const auto& entry) { return entry.node == node.id; });
    if (found == mapping.stages().end())
      add(report, StageAssignmentDiagnosticCode::STAGE_MISSING_STAGE, "TargetDFG node has no stage",
          node.id);
  }
  if (!report.ok())
    return report;

  std::map<NodeId, std::uint64_t> logicalTimes;
  for (const auto& node : dfg.nodes()) {
    try {
      logicalTimes.emplace(node.id, mapping.logicalIssueTime(node.id));
    } catch (const std::overflow_error& error) {
      add(report, StageAssignmentDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW, error.what(),
          node.id);
    } catch (const std::out_of_range& error) {
      add(report, StageAssignmentDiagnosticCode::STAGE_MISSING_STAGE, error.what(), node.id);
    }
  }
  if (!report.ok())
    return report;

  const auto ii = static_cast<std::uint64_t>(mapping.modulo().ii());
  if (ii == 0) {
    add(report, StageAssignmentDiagnosticCode::STAGE_INVALID_MODULO_MAPPING,
        "modulo mapping II must be positive");
    return report;
  }
  for (const auto& edge : dfg.edges()) {
    const auto src = logicalTimes.at(edge.src);
    const auto dst = logicalTimes.at(edge.dst);
    std::uint64_t lhs = 0;
    std::uint64_t rhs = 0;
    if (!checkedMultiplyAdd(edge.distance, ii, dst, lhs)) {
      add(report, StageAssignmentDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW,
          "logical dependence time overflows uint64", std::nullopt, edge.id);
      continue;
    }
    rhs = src;
    const auto& dependence = mapping.modulo().dependence(edge.id);
    if (rhs > std::numeric_limits<std::uint64_t>::max() - dependence.requiredSeparationCycles) {
      add(report, StageAssignmentDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW,
          "dependence separation addition overflows uint64", std::nullopt, edge.id);
      continue;
    }
    rhs += dependence.requiredSeparationCycles;
    if (lhs < rhs) {
      add(report, StageAssignmentDiagnosticCode::STAGE_CONSTRAINT_VIOLATION,
          "staged logical-time dependence constraint is violated", std::nullopt, edge.id);
    }
  }
  return report;
}

} // namespace cgra::schedule
