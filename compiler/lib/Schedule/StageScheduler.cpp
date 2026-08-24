// SPDX-License-Identifier: MIT
#include "cgra/Schedule/StageScheduler.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace cgra::schedule {
namespace {

using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

void add(StageSchedulingResult& result, StageSchedulingDiagnosticCode code, std::string message,
         std::optional<NodeId> node = std::nullopt, std::optional<EdgeId> edge = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), node, edge});
}

bool checkedSignedAdd(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))
    return false;
  result = lhs + rhs;
  return true;
}

std::optional<StageConstraintCycleWitness>
witnessFor(const std::vector<NodeId>& nodes,
           const std::vector<std::optional<std::size_t>>& predecessor,
           const std::vector<std::optional<EdgeId>>& predecessorEdge, std::size_t updated,
           const std::map<EdgeId, StageConstraint>& byEdge) {
  if (nodes.empty() || !predecessor[updated] || !predecessorEdge[updated])
    return std::nullopt;
  auto current = updated;
  for (std::size_t count = 0; count < nodes.size(); ++count) {
    if (!predecessor[current] || !predecessorEdge[current])
      return std::nullopt;
    current = *predecessor[current];
  }
  const auto cycleStart = current;
  std::vector<NodeId> cycleNodes;
  std::vector<EdgeId> cycleEdges;
  do {
    if (!predecessor[current] || !predecessorEdge[current])
      return std::nullopt;
    cycleNodes.push_back(nodes[current]);
    cycleEdges.push_back(*predecessorEdge[current]);
    current = *predecessor[current];
  } while (current != cycleStart && cycleNodes.size() <= nodes.size());
  if (current != cycleStart || cycleNodes.empty())
    return std::nullopt;

  auto minimum = std::min_element(cycleNodes.begin(), cycleNodes.end());
  const auto rotateBy = static_cast<std::size_t>(minimum - cycleNodes.begin());
  std::rotate(cycleNodes.begin(), cycleNodes.begin() + rotateBy, cycleNodes.end());
  std::rotate(cycleEdges.begin(), cycleEdges.begin() + rotateBy, cycleEdges.end());

  std::int64_t total = 0;
  for (const auto edge : cycleEdges) {
    const auto found = byEdge.find(edge);
    if (found == byEdge.end())
      return std::nullopt;
    if (!checkedSignedAdd(total, found->second.minimumStageDelta, total))
      return std::nullopt;
  }
  return StageConstraintCycleWitness{std::move(cycleNodes), std::move(cycleEdges), total};
}

} // namespace

std::int64_t ceilDivSigned(std::int64_t numerator, std::int64_t positiveDenominator) {
  if (positiveDenominator <= 0)
    throw std::invalid_argument("signed ceil division requires a positive denominator");
  const auto quotient = numerator / positiveDenominator;
  const auto remainder = numerator % positiveDenominator;
  if (remainder != 0 && numerator > 0) {
    if (quotient == std::numeric_limits<std::int64_t>::max())
      throw std::overflow_error("signed ceil division overflows int64");
    return quotient + 1;
  }
  return quotient;
}

StageSchedulingResult StageScheduler::schedule(const cgra::target::TargetDFG& dfg,
                                               const cgra::TargetModel& target,
                                               const cgra::mapping::ModuloMapping& mapping) {
  StageSchedulingResult result;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    result.status = StageSchedulingStatus::InvalidTargetDFG;
    add(result, StageSchedulingDiagnosticCode::STAGE_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return result;
  }
  if (dfg.targetName() != target.name()) {
    result.status = StageSchedulingStatus::InvalidTargetDFG;
    add(result, StageSchedulingDiagnosticCode::STAGE_INVALID_TARGET_DFG,
        "TargetDFG target name does not match selected TargetModel");
    return result;
  }
  if (mapping.ii() == 0) {
    result.status = StageSchedulingStatus::InvalidModuloMapping;
    add(result, StageSchedulingDiagnosticCode::STAGE_INVALID_MODULO_MAPPING,
        "modulo mapping II must be at least one");
    return result;
  }
  const auto moduloReport = cgra::mapping::ModuloMappingVerifier::verify(dfg, target, mapping);
  if (!moduloReport.ok()) {
    result.status = StageSchedulingStatus::InvalidModuloMapping;
    add(result, StageSchedulingDiagnosticCode::STAGE_INVALID_MODULO_MAPPING,
        "ModuloMapping precondition failed: " + moduloReport.format());
    return result;
  }

  std::vector<NodeId> nodes;
  nodes.reserve(dfg.nodes().size());
  for (const auto& node : dfg.nodes())
    nodes.push_back(node.id);
  std::sort(nodes.begin(), nodes.end());
  std::map<NodeId, std::size_t> nodeIndices;
  for (std::size_t index = 0; index < nodes.size(); ++index)
    nodeIndices.emplace(nodes[index], index);

  std::vector<StageConstraint> constraints;
  constraints.reserve(dfg.edges().size());
  std::map<EdgeId, StageConstraint> constraintByEdge;
  try {
    for (const auto& edge : dfg.edges()) {
      const auto source = mapping.placement(edge.src);
      const auto destination = mapping.placement(edge.dst);
      const auto dependence = mapping.dependence(edge.id);
      const auto slotDifference = static_cast<std::int64_t>(destination.issueSlot.value()) -
                                  static_cast<std::int64_t>(source.issueSlot.value());
      const auto numerator =
          static_cast<std::int64_t>(dependence.requiredSeparationCycles) - slotDifference;
      auto delta = ceilDivSigned(numerator, static_cast<std::int64_t>(mapping.ii()));
      if (static_cast<std::int64_t>(edge.distance) > delta &&
          delta < std::numeric_limits<std::int64_t>::min() + edge.distance) {
        result.status = StageSchedulingStatus::ArithmeticOverflow;
        add(result, StageSchedulingDiagnosticCode::STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW,
            "stage constraint distance subtraction overflows int64", std::nullopt, edge.id);
        return result;
      }
      delta -= static_cast<std::int64_t>(edge.distance);
      const StageConstraint constraint{edge.id, edge.src,      edge.dst,
                                       delta,   edge.distance, dependence.requiredSeparationCycles};
      constraints.push_back(constraint);
      constraintByEdge.emplace(edge.id, constraint);
    }
  } catch (const std::overflow_error& error) {
    result.status = StageSchedulingStatus::ArithmeticOverflow;
    add(result, StageSchedulingDiagnosticCode::STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW, error.what());
    return result;
  } catch (const std::out_of_range& error) {
    result.status = StageSchedulingStatus::InvalidModuloMapping;
    add(result, StageSchedulingDiagnosticCode::STAGE_INVALID_MODULO_MAPPING, error.what());
    return result;
  }
  std::sort(constraints.begin(), constraints.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.edge < rhs.edge; });
  result.stats.constraints = constraints.size();

  std::vector<std::uint64_t> stages(nodes.size(), 0);
  std::vector<std::optional<std::size_t>> predecessor(nodes.size());
  std::vector<std::optional<EdgeId>> predecessorEdge(nodes.size());
  std::optional<std::size_t> lastUpdated;
  for (std::size_t round = 0; round < nodes.size(); ++round) {
    bool changed = false;
    lastUpdated.reset();
    for (const auto& constraint : constraints) {
      const auto sourceIndex = nodeIndices.at(constraint.src);
      const auto destinationIndex = nodeIndices.at(constraint.dst);
      if (stages[sourceIndex] >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result.status = StageSchedulingStatus::ArithmeticOverflow;
        add(result, StageSchedulingDiagnosticCode::STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW,
            "stage relaxation overflows uint64", constraint.dst, constraint.edge);
        return result;
      }
      std::int64_t signedCandidate = 0;
      if (!checkedSignedAdd(static_cast<std::int64_t>(stages[sourceIndex]),
                            constraint.minimumStageDelta, signedCandidate)) {
        result.status = StageSchedulingStatus::ArithmeticOverflow;
        add(result, StageSchedulingDiagnosticCode::STAGE_CONSTRAINT_ARITHMETIC_OVERFLOW,
            "stage relaxation overflows int64", constraint.dst, constraint.edge);
        return result;
      }
      if (signedCandidate > 0 &&
          static_cast<std::uint64_t>(signedCandidate) > stages[destinationIndex]) {
        stages[destinationIndex] = static_cast<std::uint64_t>(signedCandidate);
        predecessor[destinationIndex] = sourceIndex;
        predecessorEdge[destinationIndex] = constraint.edge;
        changed = true;
        lastUpdated = destinationIndex;
        ++result.stats.successfulRelaxations;
      }
    }
    result.stats.relaxationRounds = round + 1;
    if (!changed)
      break;
    if (round + 1 == nodes.size() && lastUpdated) {
      result.status = StageSchedulingStatus::InfeasibleStageConstraints;
      add(result, StageSchedulingDiagnosticCode::STAGE_INFEASIBLE_POSITIVE_CYCLE,
          "positive stage-constraint cycle makes this mapping infeasible");
      result.witness =
          witnessFor(nodes, predecessor, predecessorEdge, *lastUpdated, constraintByEdge);
      return result;
    }
  }

  std::vector<NodeStage> assignments;
  assignments.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index)
    assignments.push_back({nodes[index], stages[index]});
  try {
    StagedMapping staged(mapping, assignments);
    result.stats.maxStage = staged.maxStage();
    for (const auto& node : dfg.nodes())
      result.stats.maxLogicalIssueTime =
          std::max(result.stats.maxLogicalIssueTime, staged.logicalIssueTime(node.id));
    const auto verification = StageAssignmentVerifier::verify(dfg, target, staged);
    if (!verification.ok()) {
      result.status = StageSchedulingStatus::VerificationFailure;
      add(result, StageSchedulingDiagnosticCode::STAGE_FINAL_VERIFICATION_FAILED,
          verification.format());
      return result;
    }
    result.mapping = std::move(staged);
    result.status = StageSchedulingStatus::Success;
    return result;
  } catch (const std::overflow_error& error) {
    result.status = StageSchedulingStatus::ArithmeticOverflow;
    add(result, StageSchedulingDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW, error.what());
  } catch (const std::exception& error) {
    result.status = StageSchedulingStatus::InternalError;
    add(result, StageSchedulingDiagnosticCode::STAGE_INTERNAL_ERROR, error.what());
  }
  return result;
}

} // namespace cgra::schedule
