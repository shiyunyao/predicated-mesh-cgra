// SPDX-License-Identifier: MIT
#include "cgra/Analysis/MIIAnalyzer.h"

#include "cgra/Analysis/RecurrenceConstraint.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>

namespace cgra::analysis {
namespace {

using NodeId = target::TargetNodeId;
using EdgeId = target::TargetEdgeId;
using WideInt = __int128_t;

struct IndexedConstraint {
  RecurrenceConstraint constraint;
  std::size_t srcIndex = 0;
  std::size_t dstIndex = 0;
};

struct FeasibilityResult {
  bool feasible = true;
  std::optional<RecurrenceWitness> witness;
  std::optional<std::size_t> lastUpdated;
  std::vector<std::size_t> predecessorNode;
  std::vector<std::size_t> predecessorConstraint;
};

void addDiagnostic(MIIResult& result, MIIAnalysisDiagnosticCode code, std::string message,
                   std::optional<NodeId> node = std::nullopt,
                   std::optional<EdgeId> edge = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), node, edge});
}

bool checkedAdd(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

std::optional<std::uint32_t> ceilDivToMII(std::uint64_t numerator, std::uint64_t denominator) {
  if (numerator == 0)
    return 1;
  if (denominator == 0)
    return std::nullopt;
  const auto quotient = numerator / denominator;
  const auto rounded = quotient + (numerator % denominator == 0 ? 0 : 1);
  if (rounded > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return static_cast<std::uint32_t>(rounded);
}

std::optional<RecurrenceWitness> buildWitness(const FeasibilityResult& feasibility,
                                              const std::vector<IndexedConstraint>& constraints,
                                              std::size_t nodeCount) {
  if (!feasibility.lastUpdated || feasibility.predecessorNode.size() != nodeCount)
    return std::nullopt;
  constexpr auto invalid = std::numeric_limits<std::size_t>::max();
  auto cycleNode = *feasibility.lastUpdated;
  for (std::size_t count = 0; count < nodeCount; ++count) {
    if (cycleNode >= feasibility.predecessorNode.size() ||
        feasibility.predecessorNode[cycleNode] == invalid)
      return std::nullopt;
    cycleNode = feasibility.predecessorNode[cycleNode];
  }

  std::vector<std::size_t> cycleConstraints;
  auto current = cycleNode;
  do {
    if (current >= feasibility.predecessorConstraint.size() ||
        feasibility.predecessorConstraint[current] == invalid)
      return std::nullopt;
    cycleConstraints.push_back(feasibility.predecessorConstraint[current]);
    current = feasibility.predecessorNode[current];
    if (cycleConstraints.size() > nodeCount + 1)
      return std::nullopt;
  } while (current != cycleNode);

  std::ranges::sort(cycleConstraints, [&constraints](std::size_t lhs, std::size_t rhs) {
    return constraints[lhs].constraint.edge < constraints[rhs].constraint.edge;
  });
  RecurrenceWitness witness;
  for (const auto index : cycleConstraints) {
    const auto& constraint = constraints[index].constraint;
    witness.edges.push_back(constraint.edge);
    if (!checkedAdd(witness.totalSeparation, constraint.intrinsicSeparation,
                    witness.totalSeparation) ||
        !checkedAdd(witness.totalDistance, constraint.distance, witness.totalDistance))
      return std::nullopt;
  }
  return witness;
}

FeasibilityResult evaluate(const std::vector<IndexedConstraint>& constraints, std::size_t nodeCount,
                           std::uint64_t ii) {
  constexpr auto invalid = std::numeric_limits<std::size_t>::max();
  FeasibilityResult result;
  result.predecessorNode.assign(nodeCount, invalid);
  result.predecessorConstraint.assign(nodeCount, invalid);
  std::vector<WideInt> longest(nodeCount, 0);
  for (std::size_t round = 0; round < nodeCount; ++round) {
    result.lastUpdated.reset();
    for (std::size_t index = 0; index < constraints.size(); ++index) {
      const auto& constraint = constraints[index];
      const auto weight =
          static_cast<WideInt>(constraint.constraint.intrinsicSeparation) -
          static_cast<WideInt>(constraint.constraint.distance) * static_cast<WideInt>(ii);
      const auto candidate = longest[constraint.srcIndex] + weight;
      if (longest[constraint.dstIndex] < candidate) {
        longest[constraint.dstIndex] = candidate;
        result.predecessorNode[constraint.dstIndex] = constraint.srcIndex;
        result.predecessorConstraint[constraint.dstIndex] = index;
        result.lastUpdated = constraint.dstIndex;
      }
    }
    if (!result.lastUpdated) {
      result.feasible = true;
      return result;
    }
  }
  result.feasible = false;
  result.witness = buildWitness(result, constraints, nodeCount);
  return result;
}

bool allDiagnosticsAreNoResource(const target::TargetDFGVerificationReport& report) {
  const auto diagnostics = report.diagnostics();
  return !diagnostics.empty() && std::ranges::all_of(diagnostics, [](const auto& diagnostic) {
    return diagnostic.code ==
           target::TargetDFGDiagnosticCode::TDFG_NO_COMPATIBLE_EXECUTION_RESOURCE;
  });
}

} // namespace

MIIResult MIIAnalyzer::analyze(const target::TargetDFG& dfg, const TargetModel& target) {
  MIIResult result;
  result.status = MIIStatus::Success;
  const auto targetReport = target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    const auto status = allDiagnosticsAreNoResource(targetReport) ? MIIStatus::NoCompatibleResource
                                                                  : MIIStatus::InvalidTargetDFG;
    result.status = status;
    addDiagnostic(result,
                  status == MIIStatus::NoCompatibleResource
                      ? MIIAnalysisDiagnosticCode::MII_NO_COMPATIBLE_RESOURCE
                      : MIIAnalysisDiagnosticCode::MII_INVALID_TARGET_DFG,
                  "TargetDFG precondition failed: " + targetReport.format());
    return result;
  }
  if (dfg.targetName() != target.name()) {
    result.status = MIIStatus::InvalidTargetDFG;
    addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_INVALID_TARGET_DFG,
                  "TargetDFG target name does not match selected TargetModel");
    return result;
  }

  const std::uint64_t fuCapacity = target.executionResourceCount(TargetExecutionClass::FU);
  const std::uint64_t lsuCapacity = target.executionResourceCount(TargetExecutionClass::LSU);

  std::uint64_t fuDemand = 0;
  std::uint64_t lsuDemand = 0;
  std::uint64_t selfOccupancy = 1;
  std::uint64_t totalOperationSeparation = 0;
  std::map<std::string, std::uint64_t> operationDemand;
  for (const auto& node : dfg.nodes()) {
    if (node.issueOccupancy == 0) {
      result.status = MIIStatus::TargetContractError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_TARGET_OCCUPANCY_INVALID,
                    "target operation issue occupancy must be at least one", node.id);
      continue;
    }
    const auto* operation = target.findOperation(node.operation);
    if (!operation) {
      result.status = MIIStatus::InvalidTargetDFG;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_INVALID_TARGET_DFG,
                    "TargetDFG references an operation absent from TargetModel", node.id);
      continue;
    }
    const auto demand = static_cast<std::uint64_t>(node.issueOccupancy);
    auto& perOperation = operationDemand[node.operation];
    if (!checkedAdd(perOperation, demand, perOperation)) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                    "operation demand overflowed uint64_t", node.id);
      continue;
    }
    if (node.executionClass == TargetExecutionClass::FU) {
      if (!checkedAdd(fuDemand, demand, fuDemand)) {
        result.status = MIIStatus::InternalError;
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                      "FU demand overflowed uint64_t", node.id);
      }
    } else if (!checkedAdd(lsuDemand, demand, lsuDemand)) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                    "LSU demand overflowed uint64_t", node.id);
    }
    selfOccupancy = std::max(selfOccupancy, demand);
  }
  if (!result.diagnostics.empty() && result.status != MIIStatus::Success) {
    if (result.status == MIIStatus::InvalidTargetDFG ||
        result.status == MIIStatus::TargetContractError ||
        result.status == MIIStatus::InternalError)
      return result;
  }

  if ((fuDemand > 0 && fuCapacity == 0) || (lsuDemand > 0 && lsuCapacity == 0)) {
    result.status = MIIStatus::NoCompatibleResource;
    addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_NO_COMPATIBLE_RESOURCE,
                  "target has demand but no compatible execution resource");
    return result;
  }

  const auto selfBound = ceilDivToMII(selfOccupancy, 1);
  const auto fuBound = ceilDivToMII(fuDemand, fuCapacity);
  const auto lsuBound = ceilDivToMII(lsuDemand, lsuCapacity);
  if (!selfBound || !fuBound || !lsuBound) {
    result.status = MIIStatus::InternalError;
    addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                  "resource MII does not fit uint32_t");
    return result;
  }
  result.resourceBreakdown.selfOccupancyMII = std::max<std::uint32_t>(1, *selfBound);
  result.resourceBreakdown.fuMII = std::max<std::uint32_t>(1, *fuBound);
  result.resourceBreakdown.lsuMII = std::max<std::uint32_t>(1, *lsuBound);
  result.resourceBreakdown.perOperationMII = 1;
  for (const auto& [operationName, demand] : operationDemand) {
    const auto capacity = target.compatibleResourceCount(target.operation(operationName));
    if (capacity == 0) {
      result.status = MIIStatus::NoCompatibleResource;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_NO_COMPATIBLE_RESOURCE,
                    "target operation has no compatible execution resource");
      continue;
    }
    const auto bound = ceilDivToMII(demand, capacity);
    if (!bound) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                    "per-operation MII does not fit uint32_t");
      return result;
    }
    result.resourceBreakdown.perOperationMII =
        std::max(result.resourceBreakdown.perOperationMII, *bound);
  }
  if (result.status == MIIStatus::NoCompatibleResource)
    return result;
  result.resourceMII =
      std::max({1u, result.resourceBreakdown.selfOccupancyMII, result.resourceBreakdown.fuMII,
                result.resourceBreakdown.lsuMII, result.resourceBreakdown.perOperationMII});

  std::map<NodeId, std::size_t> nodeIndices;
  std::vector<const target::TargetNode*> nodes;
  for (const auto& node : dfg.nodes())
    nodes.push_back(&node);
  std::ranges::sort(nodes, [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });
  for (std::size_t index = 0; index < nodes.size(); ++index)
    nodeIndices.emplace(nodes[index]->id, index);

  std::vector<const target::TargetEdge*> edges;
  for (const auto& edge : dfg.edges())
    edges.push_back(&edge);
  std::ranges::sort(edges, [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });

  std::vector<IndexedConstraint> constraints;
  constraints.reserve(edges.size());
  for (const auto* edge : edges) {
    std::uint64_t separation = 0;
    if (edge->kind() == ir::Edge::Kind::Memory) {
      separation =
          target.memoryDependenceSeparation(std::get<ir::MemoryEdgeInfo>(edge->info).dependence);
      if (separation == 0) {
        result.status = MIIStatus::TargetContractError;
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_TARGET_MEMORY_TIMING_MISSING,
                      "memory dependence separation must be positive", std::nullopt, edge->id);
        continue;
      }
    } else {
      const auto& producer = dfg.node(edge->src);
      if (!producer.resultLatency) {
        result.status = MIIStatus::TargetContractError;
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_TARGET_LATENCY_MISSING,
                      "value edge producer has no target result latency", edge->src, edge->id);
        continue;
      }
      separation = *producer.resultLatency;
      if (separation == 0) {
        result.status = MIIStatus::TargetContractError;
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_TARGET_LATENCY_MISSING,
                      "value edge producer result latency must be positive", edge->src, edge->id);
        continue;
      }
    }
    const auto srcIt = nodeIndices.find(edge->src);
    const auto dstIt = nodeIndices.find(edge->dst);
    if (srcIt == nodeIndices.end() || dstIt == nodeIndices.end()) {
      result.status = MIIStatus::InvalidTargetDFG;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_INVALID_TARGET_DFG,
                    "recurrence edge references an unknown node", std::nullopt, edge->id);
      continue;
    }
    constraints.push_back(
        {{edge->id, edge->src, edge->dst, edge->distance, static_cast<std::uint32_t>(separation)},
         srcIt->second,
         dstIt->second});
    if (!checkedAdd(totalOperationSeparation, separation, totalOperationSeparation)) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                    "recurrence separation sum overflowed uint64_t", std::nullopt, edge->id);
    }
  }
  if (result.status == MIIStatus::TargetContractError ||
      result.status == MIIStatus::InvalidTargetDFG || result.status == MIIStatus::InternalError)
    return result;

  if (constraints.empty()) {
    result.recurrenceMII = 1;
  } else {
    std::vector<IndexedConstraint> zeroDistance;
    for (const auto& constraint : constraints)
      if (constraint.constraint.distance == 0)
        zeroDistance.push_back(constraint);
    const auto zeroFeasibility = evaluate(zeroDistance, nodes.size(), 0);
    if (!zeroFeasibility.feasible) {
      result.status = MIIStatus::UnschedulableZeroDistanceCycle;
      result.recurrenceWitness = zeroFeasibility.witness;
      if (result.recurrenceWitness && !result.recurrenceWitness->edges.empty())
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ZERO_DISTANCE_CYCLE,
                      "directed zero-distance recurrence cycle has positive intrinsic separation",
                      std::nullopt, result.recurrenceWitness->edges.front());
      else
        addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ZERO_DISTANCE_CYCLE,
                      "directed zero-distance recurrence cycle has positive intrinsic separation");
      return result;
    }

    if (totalOperationSeparation == 0) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_INTERNAL_ERROR,
                    "non-empty recurrence constraints have zero total separation");
      return result;
    }
    if (totalOperationSeparation > std::numeric_limits<std::uint32_t>::max()) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_ARITHMETIC_OVERFLOW,
                    "recurrence MII upper bound does not fit uint32_t");
      return result;
    }
    std::uint64_t low = 1;
    std::uint64_t high = std::max<std::uint64_t>(1, totalOperationSeparation);
    if (!evaluate(constraints, nodes.size(), high).feasible) {
      result.status = MIIStatus::InternalError;
      addDiagnostic(result, MIIAnalysisDiagnosticCode::MII_INTERNAL_ERROR,
                    "recurrence feasibility remained unsatisfied at the safe upper bound");
      return result;
    }
    while (low < high) {
      const auto mid = low + (high - low) / 2;
      if (evaluate(constraints, nodes.size(), mid).feasible)
        high = mid;
      else
        low = mid + 1;
    }
    result.recurrenceMII = static_cast<std::uint32_t>(low);
    if (result.recurrenceMII > 1)
      result.recurrenceWitness =
          evaluate(constraints, nodes.size(), result.recurrenceMII - 1).witness;
  }

  result.status = MIIStatus::Success;
  result.mii = std::max(result.resourceMII, result.recurrenceMII);
  if (result.mii == 0)
    result.mii = 1;
  return result;
}

} // namespace cgra::analysis
