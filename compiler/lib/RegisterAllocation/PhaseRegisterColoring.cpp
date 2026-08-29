// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/PhaseRegisterColoring.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <vector>

namespace cgra::register_allocation {
namespace {

bool boundaryReuseAllowed(cgra::SameAddressReadWritePolicy policy) {
  return policy == cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew;
}

bool intervalsConflict(std::int64_t lhsWrite, std::int64_t lhsRead,
                       std::int64_t rhsWrite, std::int64_t rhsRead,
                       cgra::SameAddressReadWritePolicy policy) {
  const auto start = std::max(lhsWrite, rhsWrite);
  const auto end = std::min(lhsRead, rhsRead);
  return start < end || (start == end && !boundaryReuseAllowed(policy));
}

bool phaseConflict(const StorageSegment& lhs, std::uint32_t lhsPhase,
                   std::uint32_t lhsPhaseCount, const StorageSegment& rhs,
                   std::uint32_t rhsPhase, std::uint32_t rhsPhaseCount,
                   std::uint32_t ii, cgra::SameAddressReadWritePolicy policy) {
  if (ii == 0 || lhsPhaseCount == 0 || rhsPhaseCount == 0 ||
      lhs.readTime <= lhs.writeTime || rhs.readTime <= rhs.writeTime)
    return true;
  if (lhs.id == rhs.id && lhsPhase != rhsPhase)
    return true;
  const auto gcd = std::gcd(lhsPhaseCount, rhsPhaseCount);
  const auto lcm = static_cast<std::uint64_t>(lhsPhaseCount / gcd) * rhsPhaseCount;
  if (lcm == 0 || lcm > 1000000)
    return true;
  const auto window = static_cast<std::int64_t>(lcm);
  const auto lhsOffset = static_cast<std::int64_t>(lhsPhase) -
                         static_cast<std::int64_t>(lhsPhaseCount);
  const auto rhsOffset = static_cast<std::int64_t>(rhsPhase) -
                         static_cast<std::int64_t>(rhsPhaseCount);
  for (std::int64_t lhsIteration = lhsOffset; lhsIteration < window + lhsOffset;
       lhsIteration += lhsPhaseCount) {
    const auto lhsWrite = static_cast<std::int64_t>(lhs.writeTime) + lhsIteration * ii;
    const auto lhsRead = static_cast<std::int64_t>(lhs.readTime) + lhsIteration * ii;
    for (std::int64_t rhsIteration = rhsOffset - window;
         rhsIteration < 2 * window + rhsOffset; rhsIteration += rhsPhaseCount) {
      const auto rhsWrite = static_cast<std::int64_t>(rhs.writeTime) + rhsIteration * ii;
      const auto rhsRead = static_cast<std::int64_t>(rhs.readTime) + rhsIteration * ii;
      if (intervalsConflict(lhsWrite, lhsRead, rhsWrite, rhsRead, policy))
        return true;
    }
  }
  return false;
}

struct Group {
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankDomain domain;
  std::vector<PhaseVertex> vertices;
};

struct BankKey {
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankId bank;
  friend bool operator<(const BankKey& lhs, const BankKey& rhs) {
    return std::tie(lhs.tile.row, lhs.tile.col, lhs.bank) <
           std::tie(rhs.tile.row, rhs.tile.col, rhs.bank);
  }
};

bool colorExact(const std::vector<PhaseVertex>& vertices,
                const std::vector<std::vector<bool>>& graph,
                const cgra::RegisterFileDesc& bank,
                std::map<PhaseVertex, std::uint32_t>& colors,
                PhaseRegisterColoringResult& result,
                const RFAllocationBudget& budget) {
  std::optional<std::size_t> selected;
  std::size_t bestSaturation = 0;
  std::size_t bestDegree = 0;
  for (std::size_t index = 0; index < vertices.size(); ++index) {
    if (colors.contains(vertices[index]))
      continue;
    std::set<std::uint32_t> saturation;
    std::size_t degree = 0;
    for (std::size_t neighbor = 0; neighbor < vertices.size(); ++neighbor) {
      if (!graph[index][neighbor])
        continue;
      ++degree;
      if (const auto found = colors.find(vertices[neighbor]); found != colors.end())
        saturation.insert(found->second);
    }
    if (!selected || saturation.size() > bestSaturation ||
        (saturation.size() == bestSaturation &&
         (degree > bestDegree ||
          (degree == bestDegree && vertices[index] < vertices[*selected])))) {
      selected = index;
      bestSaturation = saturation.size();
      bestDegree = degree;
    }
  }
  if (!selected)
    return true;

  for (const auto color : bank.allocatableIndices) {
    bool available = true;
    for (std::size_t neighbor = 0; neighbor < vertices.size(); ++neighbor) {
      const auto found = colors.find(vertices[neighbor]);
      if (graph[*selected][neighbor] && found != colors.end() && found->second == color) {
        available = false;
        break;
      }
    }
    if (!available)
      continue;
    if (result.decisions >= budget.maxColoringDecisions) {
      result.status = PhaseRegisterColoringResult::Status::BudgetExceeded;
      return false;
    }
    ++result.decisions;
    colors.emplace(vertices[*selected], color);
    if (colorExact(vertices, graph, bank, colors, result, budget))
      return true;
    colors.erase(vertices[*selected]);
    if (result.status == PhaseRegisterColoringResult::Status::BudgetExceeded)
      return false;
    if (result.backtracks >= budget.maxColoringBacktracks) {
      result.status = PhaseRegisterColoringResult::Status::BudgetExceeded;
      return false;
    }
    ++result.backtracks;
  }
  result.status = PhaseRegisterColoringResult::Status::RegisterDepthInfeasible;
  return false;
}

} // namespace

bool phaseVerticesConflict(const StorageSegment& lhs, std::uint32_t lhsPhase,
                           std::uint32_t lhsPhaseCount, const StorageSegment& rhs,
                           std::uint32_t rhsPhase, std::uint32_t rhsPhaseCount,
                           std::uint32_t ii, cgra::SameAddressReadWritePolicy policy) {
  return phaseConflict(lhs, lhsPhase, lhsPhaseCount, rhs, rhsPhase, rhsPhaseCount, ii, policy);
}

PhaseRegisterColoringResult colorPhaseRegisters(
    std::span<const StorageSegment> segments, const RotationPlan& rotation,
    const TargetModel& target, const RFAllocationBudget& budget) {
  PhaseRegisterColoringResult result;
  if (!rotation.ok() || rotation.segments.size() != segments.size())
    return result;
  std::map<BankKey, Group> groups;
  for (const auto& requirement : rotation.segments) {
    if (requirement.segment >= segments.size() || requirement.minimumPhaseCount == 0)
      return result;
    const auto& segment = segments[requirement.segment];
    const auto* bank = target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
    if (!bank || bank->allocatableIndices.size() < requirement.minimumPhaseCount) {
      result.status = PhaseRegisterColoringResult::Status::RegisterDepthInfeasible;
      return result;
    }
    const BankKey key{segment.tile, bank->id};
    auto& group = groups[key];
    group.tile = segment.tile;
    group.domain = segment.domain;
    for (std::uint32_t phase = 0; phase < requirement.minimumPhaseCount; ++phase)
      group.vertices.push_back({segment.id, phase});
  }

  for (auto& [key, group] : groups) {
    const auto* bank = target.registerBank(group.domain, group.tile.row, group.tile.col);
    if (!bank)
      return result;
    std::vector<std::vector<bool>> graph(group.vertices.size(),
                                         std::vector<bool>(group.vertices.size(), false));
    for (std::size_t lhs = 0; lhs < group.vertices.size(); ++lhs) {
      for (std::size_t rhs = lhs + 1; rhs < group.vertices.size(); ++rhs) {
        const auto& lhsRequirement = rotation.segments[group.vertices[lhs].segment];
        const auto& rhsRequirement = rotation.segments[group.vertices[rhs].segment];
        graph[lhs][rhs] = graph[rhs][lhs] = phaseConflict(
            segments[group.vertices[lhs].segment], group.vertices[lhs].phase,
            lhsRequirement.minimumPhaseCount, segments[group.vertices[rhs].segment],
            group.vertices[rhs].phase, rhsRequirement.minimumPhaseCount,
            lhsRequirement.ii, bank->sameAddressReadWritePolicy);
      }
    }
    if (!colorExact(group.vertices, graph, *bank, result.colors, result, budget))
      return result;
  }
  result.status = PhaseRegisterColoringResult::Status::Success;
  return result;
}

} // namespace cgra::register_allocation
