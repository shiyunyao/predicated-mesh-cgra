// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"

#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cgra::register_allocation {
namespace {

using NodeId = cgra::target::TargetNodeId;
using EdgeId = cgra::target::TargetEdgeId;

void add(StorageRequirementResult& result, StorageRequirementDiagnosticCode code,
         std::string message, std::optional<EdgeId> edge = std::nullopt,
         std::optional<StorageSegmentId> segment = std::nullopt) {
  result.diagnostics.push_back({code, std::move(message), edge, segment});
}

bool checkedAdd(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    return false;
  result = lhs + rhs;
  return true;
}

bool checkedMul(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    return false;
  result = lhs * rhs;
  return true;
}

cgra::RegisterBankDomain bankDomain(cgra::mapping::NetworkDomain domain) {
  return domain == cgra::mapping::NetworkDomain::Data ? cgra::RegisterBankDomain::Data
                                                      : cgra::RegisterBankDomain::Predicate;
}

} // namespace

const StorageSegment& StorageRequirements::segment(StorageSegmentId id) const {
  const auto found = std::find_if(segments_.begin(), segments_.end(),
                                  [id](const auto& segment) { return segment.id == id; });
  if (found == segments_.end())
    throw std::out_of_range("unknown storage segment");
  return *found;
}

bool StorageRequirements::operator==(const StorageRequirements& other) const noexcept {
  return ii_ == other.ii_ && segments_ == other.segments_;
}

std::string_view toString(StorageRequirementStatus status) noexcept {
  switch (status) {
  case StorageRequirementStatus::Success:
    return "success";
  case StorageRequirementStatus::InvalidTargetDFG:
    return "invalid_target_dfg";
  case StorageRequirementStatus::InvalidStagedMapping:
    return "invalid_staged_mapping";
  case StorageRequirementStatus::TargetRFContractError:
    return "target_rf_contract_error";
  case StorageRequirementStatus::ArithmeticOverflow:
    return "arithmetic_overflow";
  case StorageRequirementStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(StorageRequirementDiagnosticCode code) noexcept {
  switch (code) {
  case StorageRequirementDiagnosticCode::RFA_INVALID_TARGET_DFG:
    return "RFA_INVALID_TARGET_DFG";
  case StorageRequirementDiagnosticCode::RFA_INVALID_STAGED_MAPPING:
    return "RFA_INVALID_STAGED_MAPPING";
  case StorageRequirementDiagnosticCode::RFA_TARGET_BANK_MISSING:
    return "RFA_TARGET_BANK_MISSING";
  case StorageRequirementDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID:
    return "RFA_TARGET_RF_CONTRACT_INVALID";
  case StorageRequirementDiagnosticCode::RFA_STORAGE_TIMING_INVALID:
    return "RFA_STORAGE_TIMING_INVALID";
  case StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW:
    return "RFA_STORAGE_ARITHMETIC_OVERFLOW";
  case StorageRequirementDiagnosticCode::RFA_INTERNAL_ERROR:
    return "RFA_INTERNAL_ERROR";
  }
  return "RFA_INTERNAL_ERROR";
}

std::string StorageRequirementResult::format() const {
  std::ostringstream output;
  output << "StorageRequirementAnalysis status=" << toString(status)
         << " segments=" << stats.storageSegments << '\n';
  for (const auto& diagnostic : diagnostics) {
    output << "  [" << toString(diagnostic.code) << "] " << diagnostic.message;
    if (diagnostic.edge)
      output << " edge=" << *diagnostic.edge;
    if (diagnostic.segment)
      output << " segment=" << *diagnostic.segment;
    output << '\n';
  }
  return output.str();
}

StorageRequirementResult
StorageRequirementAnalysis::analyze(const cgra::target::TargetDFG& dfg,
                                    const cgra::TargetModel& target,
                                    const cgra::schedule::StagedMapping& mapping) {
  StorageRequirementResult result;
  const auto targetReport = cgra::target::TargetDFGVerifier::verify(dfg, target);
  if (!targetReport.ok()) {
    result.status = StorageRequirementStatus::InvalidTargetDFG;
    add(result, StorageRequirementDiagnosticCode::RFA_INVALID_TARGET_DFG,
        "TargetDFG precondition failed: " + targetReport.format());
    return result;
  }
  const auto stageReport = cgra::schedule::StageAssignmentVerifier::verify(dfg, target, mapping);
  if (!stageReport.ok()) {
    result.status = StorageRequirementStatus::InvalidStagedMapping;
    add(result, StorageRequirementDiagnosticCode::RFA_INVALID_STAGED_MAPPING,
        "StagedMapping precondition failed: " + stageReport.format());
    return result;
  }
  const auto ii = mapping.modulo().ii();
  if (ii == 0) {
    result.status = StorageRequirementStatus::InvalidStagedMapping;
    add(result, StorageRequirementDiagnosticCode::RFA_INVALID_STAGED_MAPPING,
        "staged mapping II must be positive");
    return result;
  }

  std::vector<StorageSegment> segments;
  auto appendSegment = [&](EdgeId edge, cgra::mapping::TileCoord tile,
                           cgra::mapping::NetworkDomain networkDomain, std::uint64_t writeTime,
                           std::uint64_t readTime, StorageOrigin origin, bool allowCoalesce) {
    const auto domain = bankDomain(networkDomain);
    if (allowCoalesce && !segments.empty()) {
      auto& previous = segments.back();
      if (previous.edge == edge && previous.tile == tile && previous.domain == domain &&
          previous.readTime == writeTime) {
        previous.readTime = readTime;
        previous.origins.push_back(std::move(origin));
        ++result.stats.coalescedOrigins;
        return;
      }
    }
    const auto id = static_cast<StorageSegmentId>(segments.size());
    segments.push_back({id, edge, tile, domain, writeTime, readTime, {std::move(origin)}});
  };

  try {
    std::vector<const cgra::target::TargetEdge*> edges;
    for (const auto& edge : dfg.edges())
      edges.push_back(&edge);
    std::sort(edges.begin(), edges.end(),
              [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });
    for (const auto* edge : edges) {
      if (edge->kind() == cgra::ir::Edge::Kind::Memory)
        continue;
      const auto& dependence = mapping.modulo().dependence(edge->id);
      if (!dependence.transport) {
        result.status = StorageRequirementStatus::InvalidStagedMapping;
        add(result, StorageRequirementDiagnosticCode::RFA_INVALID_STAGED_MAPPING,
            "value edge has no transport plan", edge->id);
        return result;
      }
      const auto producerTime = mapping.logicalIssueTime(edge->src);
      const auto destinationTime = mapping.logicalIssueTime(edge->dst);
      const auto sourceDomain = dependence.transport->domain;
      bool previousWasHold = false;
      for (std::size_t actionIndex = 0; actionIndex < dependence.transport->actions.size();
           ++actionIndex) {
        const auto& action = dependence.transport->actions[actionIndex];
        if (const auto* hold = std::get_if<cgra::mapping::VirtualHold>(&action)) {
          std::uint64_t writeTime = 0;
          std::uint64_t readTime = 0;
          if (!checkedAdd(producerTime, hold->captureElapsed, writeTime) ||
              !checkedAdd(producerTime, hold->releaseElapsed, readTime)) {
            result.status = StorageRequirementStatus::ArithmeticOverflow;
            add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW,
                "VirtualHold time overflows uint64", edge->id);
            return result;
          }
          if (readTime <= writeTime) {
            result.status = StorageRequirementStatus::InvalidStagedMapping;
            add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_TIMING_INVALID,
                "VirtualHold lifetime must have positive duration", edge->id);
            return result;
          }
          appendSegment(edge->id, hold->tile, sourceDomain, writeTime, readTime,
                        {StorageOriginKind::ExplicitVirtualHold, edge->id,
                         static_cast<std::uint32_t>(actionIndex)},
                        previousWasHold);
          previousWasHold = true;
        } else {
          previousWasHold = false;
        }
      }

      std::uint64_t arrivalTime = 0;
      std::uint64_t distanceCycles = 0;
      if (!checkedAdd(producerTime, dependence.requiredSeparationCycles, arrivalTime) ||
          !checkedMul(edge->distance, ii, distanceCycles)) {
        result.status = StorageRequirementStatus::ArithmeticOverflow;
        add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW,
            "mapped dependence time overflows uint64", edge->id);
        return result;
      }
      std::uint64_t consumerUseTime = 0;
      if (!checkedAdd(destinationTime, distanceCycles, consumerUseTime)) {
        result.status = StorageRequirementStatus::ArithmeticOverflow;
        add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW,
            "consumer use time overflows uint64", edge->id);
        return result;
      }
      if (consumerUseTime < arrivalTime) {
        result.status = StorageRequirementStatus::InvalidStagedMapping;
        add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_TIMING_INVALID,
            "consumer use occurs before mapped transport arrival", edge->id);
        return result;
      }
      if (consumerUseTime > arrivalTime) {
        appendSegment(edge->id, mapping.modulo().placement(edge->dst).tile, sourceDomain,
                      arrivalTime, consumerUseTime,
                      {StorageOriginKind::TerminalSlack, edge->id, std::nullopt}, previousWasHold);
      }
    }
  } catch (const std::overflow_error& error) {
    result.status = StorageRequirementStatus::ArithmeticOverflow;
    add(result, StorageRequirementDiagnosticCode::RFA_STORAGE_ARITHMETIC_OVERFLOW, error.what());
    return result;
  } catch (const std::out_of_range& error) {
    result.status = StorageRequirementStatus::InvalidStagedMapping;
    add(result, StorageRequirementDiagnosticCode::RFA_INVALID_STAGED_MAPPING, error.what());
    return result;
  }

  for (const auto& segment : segments) {
    const auto* bank = target.registerBank(segment.domain, segment.tile.row, segment.tile.col);
    if (!bank) {
      result.status = StorageRequirementStatus::TargetRFContractError;
      add(result, StorageRequirementDiagnosticCode::RFA_TARGET_BANK_MISSING,
          "target has no register bank applicable to storage segment tile", segment.edge,
          segment.id);
      return result;
    }
    if (bank->allocatableIndices.empty() || bank->depth == 0 || bank->readPorts == 0 ||
        bank->writePorts == 0) {
      result.status = StorageRequirementStatus::TargetRFContractError;
      add(result, StorageRequirementDiagnosticCode::RFA_TARGET_RF_CONTRACT_INVALID,
          "target register bank has no allocatable capacity or ports", segment.edge, segment.id);
      return result;
    }
  }

  result.requirements = StorageRequirements(ii, std::move(segments));
  result.stats.storageSegments = result.requirements->segments().size();
  for (const auto& segment : result.requirements->segments()) {
    if (segment.domain == cgra::RegisterBankDomain::Data)
      ++result.stats.dataSegments;
    else
      ++result.stats.predicateSegments;
    for (const auto& origin : segment.origins) {
      if (origin.kind == StorageOriginKind::ExplicitVirtualHold)
        ++result.stats.explicitHoldSegments;
      else
        ++result.stats.terminalSlackSegments;
    }
  }
  result.status = StorageRequirementStatus::Success;
  return result;
}

} // namespace cgra::register_allocation
