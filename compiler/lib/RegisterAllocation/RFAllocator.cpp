// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocator.h"

#include "cgra/RegisterAllocation/PeriodicLifetime.h"
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace cgra::register_allocation {
namespace {

using SegmentIndex = std::size_t;

struct BankKey {
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankId bank;

  friend bool operator<(const BankKey& lhs, const BankKey& rhs) {
    return std::tie(lhs.tile.row, lhs.tile.col, lhs.bank) <
           std::tie(rhs.tile.row, rhs.tile.col, rhs.bank);
  }
};

void add(RFAllocationResult& result, RFAllocationDiagnosticCode code, std::string message,
         std::optional<cgra::target::TargetEdgeId> edge = std::nullopt,
         std::optional<StorageSegmentId> segment = std::nullopt,
         std::optional<cgra::mapping::TileCoord> tile = std::nullopt,
         std::optional<cgra::RegisterBankId> bank = std::nullopt,
         std::optional<std::uint32_t> reg = std::nullopt,
         std::optional<StorageSegmentId> conflictingSegment = std::nullopt) {
  result.diagnostics.push_back(
      {code, std::move(message), edge, segment, tile, bank, reg, conflictingSegment});
}

const cgra::RegisterFileDesc* bankFor(const cgra::TargetModel& target,
                                      const StorageSegment& segment) {
  return target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
}

struct BudgetState {
  bool exceeded = false;
};

bool colorExact(const std::vector<SegmentIndex>& vertices,
                const std::vector<std::vector<bool>>& graph, const cgra::RegisterFileDesc& bank,
                const RFAllocationOptions& options,
                std::vector<std::optional<std::uint32_t>>& colors, RFAllocationStats& stats,
                BudgetState& budget) {
  SegmentIndex selected = 0;
  bool found = false;
  std::size_t bestSaturation = 0;
  std::size_t bestDegree = 0;
  StorageSegmentId bestId = std::numeric_limits<StorageSegmentId>::max();
  for (const auto vertex : vertices) {
    if (colors[vertex])
      continue;
    std::set<std::uint32_t> saturation;
    std::size_t degree = 0;
    for (const auto neighbor : vertices) {
      if (!graph[vertex][neighbor])
        continue;
      ++degree;
      if (colors[neighbor])
        saturation.insert(*colors[neighbor]);
    }
    // Segment IDs are dense and stable, so this lookup is deterministic.
    const auto id = static_cast<StorageSegmentId>(vertex);
    if (!found || saturation.size() > bestSaturation ||
        (saturation.size() == bestSaturation &&
         (degree > bestDegree || (degree == bestDegree && id < bestId)))) {
      selected = vertex;
      bestSaturation = saturation.size();
      bestDegree = degree;
      bestId = id;
      found = true;
    }
  }
  if (!found)
    return true;

  for (const auto color : bank.allocatableIndices) {
    bool available = true;
    for (const auto neighbor : vertices)
      if (graph[selected][neighbor] && colors[neighbor] && *colors[neighbor] == color)
        available = false;
    if (!available)
      continue;
    if (stats.coloringDecisions >= options.budget.maxColoringDecisions) {
      budget.exceeded = true;
      return false;
    }
    ++stats.coloringDecisions;
    colors[selected] = color;
    if (colorExact(vertices, graph, bank, options, colors, stats, budget))
      return true;
    colors[selected].reset();
    if (budget.exceeded)
      return false;
    if (stats.coloringBacktracks >= options.budget.maxColoringBacktracks) {
      budget.exceeded = true;
      return false;
    }
    ++stats.coloringBacktracks;
  }
  return false;
}

} // namespace

RFAllocationResult RFAllocator::allocate(const cgra::target::TargetDFG& dfg,
                                         const cgra::TargetModel& target,
                                         const cgra::schedule::StagedMapping& mapping,
                                         const RFAllocationOptions& options) {
  RFAllocationResult result;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    result.status = RFAllocationStatus::InvalidTargetDFG;
    add(result, RFAllocationDiagnosticCode::RFA_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return result;
  }
  const auto requirementsResult = StorageRequirementAnalysis::analyze(dfg, target, mapping);
  if (!requirementsResult.ok()) {
    result.status = requirementsResult.status == StorageRequirementStatus::InvalidTargetDFG
                        ? RFAllocationStatus::InvalidTargetDFG
                    : requirementsResult.status == StorageRequirementStatus::InvalidStagedMapping
                        ? RFAllocationStatus::InvalidStagedMapping
                    : requirementsResult.status == StorageRequirementStatus::TargetRFContractError
                        ? RFAllocationStatus::TargetRFContractError
                    : requirementsResult.status == StorageRequirementStatus::ArithmeticOverflow
                        ? RFAllocationStatus::ArithmeticOverflow
                        : RFAllocationStatus::InternalError;
    for (const auto& diagnostic : requirementsResult.diagnostics)
      add(result,
          diagnostic.code == StorageRequirementDiagnosticCode::RFA_TARGET_BANK_MISSING
              ? RFAllocationDiagnosticCode::RFA_TARGET_BANK_MISSING
          : diagnostic.code == StorageRequirementDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID
              ? RFAllocationDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID
          : diagnostic.code == StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW
              ? RFAllocationDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW
          : diagnostic.code == StorageRequirementDiagnosticCode::RFA_STORAGE_TIMING_INVALID
              ? RFAllocationDiagnosticCode::RFA_STORAGE_TIMING_INVALID
          : diagnostic.code == StorageRequirementDiagnosticCode::RFA_INVALID_TARGET_DFG
              ? RFAllocationDiagnosticCode::RFA_INVALID_TARGET_DFG
              : RFAllocationDiagnosticCode::RFA_INVALID_STAGED_MAPPING,
          diagnostic.message, diagnostic.edge, diagnostic.segment);
    return result;
  }
  const auto& requirements = *requirementsResult.requirements;
  result.stats.storageSegments = requirements.segments().size();
  result.stats.dataSegments = requirementsResult.stats.dataSegments;
  result.stats.predicateSegments = requirementsResult.stats.predicateSegments;
  result.stats.explicitHoldSegments = requirementsResult.stats.explicitHoldSegments;
  result.stats.terminalSlackSegments = requirementsResult.stats.terminalSlackSegments;
  result.stats.coalescedOrigins = requirementsResult.stats.coalescedOrigins;

  const auto ii = requirements.ii();
  std::map<BankKey, std::vector<SegmentIndex>> groups;
  std::vector<const cgra::RegisterFileDesc*> banks(requirements.segments().size());
  for (const auto& segment : requirements.segments()) {
    const auto* bank = bankFor(target, segment);
    if (!bank) {
      result.status = RFAllocationStatus::TargetRFContractError;
      add(result, RFAllocationDiagnosticCode::RFA_TARGET_BANK_MISSING,
          "storage segment has no applicable target RF bank", segment.edge, segment.id,
          segment.tile);
      return result;
    }
    if (bank->allocatableIndices.empty()) {
      result.status = RFAllocationStatus::TargetRFContractError;
      add(result, RFAllocationDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID,
          "target RF bank has no allocatable register indices", segment.edge, segment.id,
          segment.tile, bank->id);
      return result;
    }
    banks[segment.id] = bank;
    groups[{segment.tile, bank->id}].push_back(segment.id);
    if (fixedRegisterSelfOverlaps(segment, ii, bank->sameAddressReadWritePolicy)) {
      result.status = RFAllocationStatus::FixedRegisterSelfOverlap;
      add(result, RFAllocationDiagnosticCode::RFA_FIXED_REGISTER_SELF_OVERLAP,
          "storage lifetime overlaps its own next II-periodic iteration", segment.edge, segment.id,
          segment.tile, bank->id);
      return result;
    }
  }

  struct PortCounts {
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;
  };
  std::map<std::tuple<BankKey, std::uint32_t>, PortCounts> ports;
  for (const auto& segment : requirements.segments()) {
    const BankKey key{segment.tile, banks[segment.id]->id};
    auto& write = ports[{key, static_cast<std::uint32_t>(segment.writeTime % ii)}];
    ++write.writes;
    auto& read = ports[{key, static_cast<std::uint32_t>(segment.readTime % ii)}];
    ++read.reads;
    result.stats.maxReadPortsUsed = std::max(result.stats.maxReadPortsUsed, read.reads);
    result.stats.maxWritePortsUsed = std::max(result.stats.maxWritePortsUsed, write.writes);
    if (write.writes > banks[segment.id]->writePorts) {
      result.status = RFAllocationStatus::WritePortConflict;
      add(result, RFAllocationDiagnosticCode::RFA_WRITE_PORT_CONFLICT,
          "periodic RF write demand exceeds target write-port capacity", segment.edge, segment.id,
          segment.tile, banks[segment.id]->id, std::nullopt);
      return result;
    }
    if (read.reads > banks[segment.id]->readPorts) {
      result.status = RFAllocationStatus::ReadPortConflict;
      add(result, RFAllocationDiagnosticCode::RFA_READ_PORT_CONFLICT,
          "periodic RF read demand exceeds target read-port capacity", segment.edge, segment.id,
          segment.tile, banks[segment.id]->id, std::nullopt);
      return result;
    }
  }

  std::vector<std::optional<std::uint32_t>> colors(requirements.segments().size());
  for (const auto& [key, vertices] : groups) {
    const auto* bank = banks[vertices.front()];
    std::vector<std::vector<bool>> graph(requirements.segments().size(),
                                         std::vector<bool>(requirements.segments().size(), false));
    for (std::size_t lhsIndex = 0; lhsIndex < vertices.size(); ++lhsIndex) {
      for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < vertices.size(); ++rhsIndex) {
        const auto lhs = vertices[lhsIndex];
        const auto rhs = vertices[rhsIndex];
        if (periodicLifetimesConflict(requirements.segment(lhs), requirements.segment(rhs), ii,
                                      bank->sameAddressReadWritePolicy)) {
          graph[lhs][rhs] = graph[rhs][lhs] = true;
          ++result.stats.conflictEdges;
        }
      }
    }

    bool greedyFailed = false;
    for (const auto vertex : vertices) {
      bool assigned = false;
      for (const auto color : bank->allocatableIndices) {
        if (result.stats.coloringDecisions >= options.budget.maxColoringDecisions) {
          result.status = RFAllocationStatus::BudgetExceeded;
          add(result, RFAllocationDiagnosticCode::RFA_COLORING_BUDGET_EXCEEDED,
              "RF coloring decision budget exhausted", requirements.segment(vertex).edge, vertex,
              key.tile, key.bank);
          return result;
        }
        bool available = true;
        for (const auto neighbor : vertices)
          if (graph[vertex][neighbor] && colors[neighbor] && *colors[neighbor] == color)
            available = false;
        if (!available)
          continue;
        ++result.stats.coloringDecisions;
        colors[vertex] = color;
        assigned = true;
        break;
      }
      if (!assigned) {
        greedyFailed = true;
        break;
      }
    }
    if (greedyFailed) {
      for (const auto vertex : vertices)
        colors[vertex].reset();
      BudgetState budget;
      if (!colorExact(vertices, graph, *bank, options, colors, result.stats, budget)) {
        if (budget.exceeded) {
          result.status = RFAllocationStatus::BudgetExceeded;
          add(result, RFAllocationDiagnosticCode::RFA_COLORING_BUDGET_EXCEEDED,
              "exact RF coloring budget exhausted", std::nullopt, std::nullopt, key.tile, key.bank);
        } else {
          result.status = RFAllocationStatus::RegisterDepthInfeasible;
          add(result, RFAllocationDiagnosticCode::RFA_REGISTER_DEPTH_INFEASIBLE,
              "RF conflict graph is not colorable with target allocatable registers", std::nullopt,
              std::nullopt, key.tile, key.bank);
        }
        return result;
      }
    }
    std::set<std::uint32_t> used;
    for (const auto vertex : vertices)
      used.insert(*colors[vertex]);
    result.stats.maxRegistersUsedOnAnyBank =
        std::max(result.stats.maxRegistersUsedOnAnyBank, static_cast<std::uint32_t>(used.size()));
  }

  std::vector<StorageAllocation> allocations;
  allocations.reserve(requirements.segments().size());
  for (const auto& segment : requirements.segments())
    allocations.push_back({segment.id, {segment.tile, banks[segment.id]->id, *colors[segment.id]}});

  struct RegisterEventCounts {
    std::uint32_t reads = 0;
    std::uint32_t writes = 0;
  };
  using RegisterEventKey =
      std::tuple<cgra::mapping::TileCoord, cgra::RegisterBankId, std::uint32_t, std::uint32_t>;
  std::map<RegisterEventKey, RegisterEventCounts> registerEvents;
  for (const auto& segment : requirements.segments()) {
    const auto& allocation = allocations[segment.id];
    ++registerEvents[{allocation.reg.tile, allocation.reg.bank, allocation.reg.index,
                      static_cast<std::uint32_t>(segment.writeTime % ii)}]
          .writes;
    ++registerEvents[{allocation.reg.tile, allocation.reg.bank, allocation.reg.index,
                      static_cast<std::uint32_t>(segment.readTime % ii)}]
          .reads;
  }
  for (const auto& [key, counts] : registerEvents) {
    const auto domain = std::get<1>(key) == target.predicateRF().id
                            ? cgra::RegisterBankDomain::Predicate
                            : cgra::RegisterBankDomain::Data;
    const auto* bank = target.registerBank(domain, std::get<0>(key).row, std::get<0>(key).col);
    if (counts.writes > 1) {
      result.status = RFAllocationStatus::SameAddressRWConflict;
      add(result, RFAllocationDiagnosticCode::RFA_SAME_ADDRESS_RW_CONFLICT,
          "multiple periodic writes target one physical RF register in one cycle", std::nullopt,
          std::nullopt, std::get<0>(key), std::get<1>(key), std::get<2>(key));
      return result;
    }
    if (counts.reads != 0 && counts.writes != 0 && bank &&
        bank->sameAddressReadWritePolicy != cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew) {
      result.status = RFAllocationStatus::SameAddressRWConflict;
      add(result, RFAllocationDiagnosticCode::RFA_SAME_ADDRESS_RW_CONFLICT,
          "same-address periodic read/write is forbidden by target RF policy", std::nullopt,
          std::nullopt, std::get<0>(key), std::get<1>(key), std::get<2>(key));
      return result;
    }
  }
  try {
    RFAllocatedMapping allocated(mapping, requirements, std::move(allocations));
    const auto verification = RFAllocationVerifier::verify(dfg, target, allocated);
    if (!verification.ok()) {
      result.status = RFAllocationStatus::VerificationFailure;
      add(result, RFAllocationDiagnosticCode::RFA_FINAL_VERIFICATION_FAILED, verification.format());
      return result;
    }
    result.mapping = std::move(allocated);
    result.status = RFAllocationStatus::Success;
  } catch (const std::exception& error) {
    result.status = RFAllocationStatus::InternalError;
    add(result, RFAllocationDiagnosticCode::RFA_INTERNAL_ERROR, error.what());
  }
  return result;
}

} // namespace cgra::register_allocation
