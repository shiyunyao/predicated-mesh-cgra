// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/RegisterAllocation/RFPortEvent.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace cgra::mapping {

enum class PartialRFEventClass : std::uint8_t {
  HoldWrite = 0,
  HoldRead = 1,
  TerminalWrite = 2,
  TerminalRead = 3,
};

std::uint64_t partialRFEventId(cgra::target::TargetEdgeId edge, std::uint32_t actionIndex,
                               PartialRFEventClass kind);

struct PartialStorageChain {
  cgra::target::TargetEdgeId edge = 0;
  std::vector<cgra::register_allocation::RFPortEvent> definiteEvents;
  bool hasDeferredFinalHoldRead = false;
  std::optional<std::uint32_t> deferredReadEarliestSlot;
};

PartialStorageChain derivePartialStorageChain(
    const cgra::target::TargetDFG& dfg, const cgra::TargetModel& target,
    const cgra::target::TargetEdge& edge, const NodePlacement& producer,
    const NodePlacement& consumer, const MappedDependence& dependence, std::uint32_t ii,
    bool reserveMandatoryTerminalEvents = true);

} // namespace cgra::mapping
