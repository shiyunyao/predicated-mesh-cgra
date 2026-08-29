// SPDX-License-Identifier: MIT
#include "cgra/Mapping/PartialRFEventAnalysis.h"

#include "cgra/Mapping/StageDifferenceAnalysis.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cgra::mapping {
namespace {

using cgra::register_allocation::RFPortEvent;
using cgra::register_allocation::RFPortEventKind;

cgra::RegisterBankDomain bankDomain(NetworkDomain domain) {
  return domain == NetworkDomain::Data ? cgra::RegisterBankDomain::Data
                                       : cgra::RegisterBankDomain::Predicate;
}

std::string incomingSource(Direction direction, cgra::RegisterBankDomain domain) {
  const auto incoming = opposite(direction);
  const char* name = incoming == Direction::North   ? "NORTH"
                     : incoming == Direction::South ? "SOUTH"
                     : incoming == Direction::East  ? "EAST"
                                                     : "WEST";
  return std::string(name) +
         (domain == cgra::RegisterBankDomain::Data ? "_DATA_IN" : "_PRED_IN");
}

std::string producerSource(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                           cgra::target::TargetNodeId producer) {
  switch (target.operation(dfg.node(producer).operation).resultSource) {
  case cgra::TargetResultSource::FuDataResult:
    return "FU_DATA_RESULT";
  case cgra::TargetResultSource::FuPredicateResult:
    return "FU_PRED_RESULT";
  case cgra::TargetResultSource::LsuLoadData:
    return "LSU_LOAD_DATA";
  case cgra::TargetResultSource::None:
    return {};
  }
  return {};
}

std::string sourceBefore(const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
                         const TransportPlan& plan, std::size_t actionIndex,
                         cgra::target::TargetNodeId producer, cgra::RegisterBankDomain domain) {
  for (std::size_t index = actionIndex; index > 0; --index) {
    if (const auto* link = std::get_if<LinkStep>(&plan.actions[index - 1]))
      return incomingSource(link->direction, domain);
    if (std::holds_alternative<VirtualHold>(plan.actions[index - 1]))
      continue;
  }
  return producerSource(dfg, target, producer);
}

RFPortEvent makeEvent(std::uint64_t id, RFPortEventKind kind, const VirtualHold& hold,
                      cgra::RegisterBankDomain domain, std::uint32_t slot,
                      std::optional<std::string> source, cgra::target::TargetEdgeId edge,
                      std::optional<std::uint32_t> action) {
  RFPortEvent event;
  event.id = id;
  event.kind = kind;
  event.tile = hold.tile;
  event.domain = domain;
  event.slot = slot;
  event.writeSource = std::move(source);
  event.edge = edge;
  event.transportActionIndex = action;
  return event;
}

} // namespace

std::uint64_t partialRFEventId(cgra::target::TargetEdgeId edge, std::uint32_t actionIndex,
                               PartialRFEventClass kind) {
  // A 32-bit edge plus a 30-bit action index and a 2-bit event class fit in a
  // u64 without the collisions caused by the old shift-by-one encoding.
  constexpr auto MaxAction = (std::numeric_limits<std::uint32_t>::max() >> 2);
  if (actionIndex != std::numeric_limits<std::uint32_t>::max() && actionIndex > MaxAction)
    throw std::overflow_error("RF event action index does not fit stable event id");
  if (actionIndex == std::numeric_limits<std::uint32_t>::max())
    actionIndex = MaxAction;
  const auto classBits = static_cast<std::uint32_t>(kind);
  if (classBits > 3)
    throw std::invalid_argument("unknown partial RF event class");
  return (static_cast<std::uint64_t>(edge) << 32) |
         (static_cast<std::uint64_t>(actionIndex) << 2) | classBits;
}

PartialStorageChain derivePartialStorageChain(
    const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
    const cgra::target::TargetEdge& edge, const NodePlacement& producer,
    const NodePlacement& consumer, const MappedDependence& dependence, std::uint32_t ii,
    bool reserveMandatoryTerminalEvents) {
  if (ii == 0)
    throw std::invalid_argument("partial storage analysis requires a non-zero II");
  PartialStorageChain chain;
  chain.edge = edge.id;
  if (!dependence.transport || edge.kind() == cgra::ir::Edge::Kind::Memory)
    return chain;

  const auto& plan = *dependence.transport;
  const auto domain = bankDomain(plan.domain);
  const ModuloTimeDomain time(ii);
  struct HoldRecord {
    std::size_t index;
    const VirtualHold* hold;
  };
  std::vector<HoldRecord> holds;
  for (std::size_t index = 0; index < plan.actions.size(); ++index)
    if (const auto* hold = std::get_if<VirtualHold>(&plan.actions[index]))
      holds.push_back({index, hold});

  StageDifferenceRequirement stage;
  try {
    stage = minimumStageDifference(edge, producer, consumer, dependence, ii);
  } catch (const std::exception&) {
    // The exact stage scheduler remains authoritative.  No terminal event is
    // mandatory when the lower-bound proof itself cannot be established.
    stage = {edge.id, 0, edge.distance, dependence.requiredSeparationCycles};
  }
  std::uint64_t minimumSlack = 0;
  try {
    minimumSlack = minimumTerminalSlackCycles(stage, producer, consumer, ii);
  } catch (const std::exception&) {
    // An unproven lower bound must remain deferred; the exact stage/RF pass
    // will decide whether terminal storage is needed.
    minimumSlack = 0;
  }
  const bool hasMandatoryTerminal = reserveMandatoryTerminalEvents && minimumSlack > 0;
  const auto lastHold = holds.empty() ? std::numeric_limits<std::size_t>::max()
                                      : holds.back().index;
  const auto terminalReadSlot = consumer.issueSlot.value();
  const auto terminalWriteSlot =
      time.advance(producer.issueSlot, dependence.requiredSeparationCycles).value();

  auto addHoldEvents = [&](std::size_t actionIndex, const VirtualHold& hold, bool includeRead) {
    const auto source = sourceBefore(dfg, target, plan, actionIndex, edge.src, domain);
    const auto captureSlot = time.advance(producer.issueSlot, hold.captureElapsed).value();
    chain.definiteEvents.push_back(makeEvent(
        partialRFEventId(edge.id, static_cast<std::uint32_t>(actionIndex),
                         PartialRFEventClass::HoldWrite),
        RFPortEventKind::PeriodicWrite, hold, domain, captureSlot, source, edge.id,
        static_cast<std::uint32_t>(actionIndex)));
    if (includeRead) {
      const auto releaseSlot = time.advance(producer.issueSlot, hold.releaseElapsed).value();
      chain.definiteEvents.push_back(makeEvent(
          partialRFEventId(edge.id, static_cast<std::uint32_t>(actionIndex),
                           PartialRFEventClass::HoldRead),
          RFPortEventKind::PeriodicRead, hold, domain, releaseSlot, std::nullopt, edge.id,
          static_cast<std::uint32_t>(actionIndex)));
    }
  };

  for (const auto& record : holds) {
    const bool finalOnConsumer = record.index == lastHold && record.hold->tile == consumer.tile;
    if (!finalOnConsumer) {
      addHoldEvents(record.index, *record.hold, true);
      continue;
    }
    if (hasMandatoryTerminal) {
      // The final hold's release and terminal arrival are the same storage
      // chain.  Keep only capture and the final consumer read.
      addHoldEvents(record.index, *record.hold, false);
      auto terminal = *record.hold;
      terminal.tile = consumer.tile;
      chain.definiteEvents.push_back(makeEvent(
          partialRFEventId(edge.id, std::numeric_limits<std::uint32_t>::max(),
                           PartialRFEventClass::TerminalRead),
          RFPortEventKind::PeriodicRead, terminal, domain, terminalReadSlot, std::nullopt, edge.id,
          std::nullopt));
    } else {
      addHoldEvents(record.index, *record.hold, false);
      chain.hasDeferredFinalHoldRead = true;
      chain.deferredReadEarliestSlot =
          time.advance(producer.issueSlot, record.hold->releaseElapsed).value();
    }
  }

  if (holds.empty() && hasMandatoryTerminal) {
    VirtualHold terminal;
    terminal.domain = plan.domain;
    terminal.tile = consumer.tile;
    chain.definiteEvents.push_back(makeEvent(
        partialRFEventId(edge.id, std::numeric_limits<std::uint32_t>::max(),
                         PartialRFEventClass::TerminalWrite),
        RFPortEventKind::PeriodicWrite, terminal, domain, terminalWriteSlot,
        sourceBefore(dfg, target, plan, plan.actions.size(), edge.src, domain), edge.id,
        std::nullopt));
    chain.definiteEvents.push_back(makeEvent(
        partialRFEventId(edge.id, std::numeric_limits<std::uint32_t>::max(),
                         PartialRFEventClass::TerminalRead),
        RFPortEventKind::PeriodicRead, terminal, domain, terminalReadSlot, std::nullopt, edge.id,
        std::nullopt));
  }
  return chain;
}

} // namespace cgra::mapping
