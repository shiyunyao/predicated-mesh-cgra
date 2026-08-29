// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFPortMatcher.h"

#include <algorithm>
#include <functional>
#include <string>

namespace cgra::register_allocation {
namespace {

bool isWrite(RFPortEventKind kind) {
  return kind == RFPortEventKind::PeriodicWrite || kind == RFPortEventKind::BoundaryWrite;
}

bool accepts(const cgra::RegisterFileDesc& bank, const RFPortEvent& event, unsigned port) {
  if (!isWrite(event.kind))
    return true;
  if (!event.writeSource)
    return false;
  const auto it = bank.writePortSources.find("W" + std::to_string(port));
  if (it == bank.writePortSources.end())
    return false;
  return std::ranges::find(it->second, *event.writeSource) != it->second.end();
}

} // namespace

RFPortMatchResult matchRFPorts(const cgra::RegisterFileDesc& bank,
                               std::span<const RFPortEvent> events) {
  RFPortMatchResult result;
  std::vector<const RFPortEvent*> ordered;
  ordered.reserve(events.size());
  for (const auto& event : events) {
    if (event.kind == RFPortEventKind::PeriodicRead && bank.readPorts == 0) {
      result.status = RFPortMatchStatus::InvalidTargetContract;
      result.conflictingEvents.push_back(event.id);
      return result;
    }
    if (isWrite(event.kind) && !event.writeSource) {
      result.status = RFPortMatchStatus::SourceCompatibilityFailure;
      result.conflictingEvents.push_back(event.id);
      return result;
    }
    ordered.push_back(&event);
  }
  std::ranges::sort(ordered, [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });

  const auto portCount = [&] {
    for (const auto* event : ordered)
      if (!isWrite(event->kind))
        return bank.readPorts;
    return bank.writePorts;
  }();
  const bool writes = !ordered.empty() && isWrite(ordered.front()->kind);
  for (const auto* event : ordered) {
    if (isWrite(event->kind) != writes) {
      result.status = RFPortMatchStatus::InvalidTargetContract;
      result.conflictingEvents.push_back(event->id);
      return result;
    }
  }
  if (ordered.empty())
    return result;
  if (portCount == 0 || ordered.size() > portCount) {
    result.status = RFPortMatchStatus::CapacityExceeded;
    for (const auto* event : ordered)
      result.conflictingEvents.push_back(event->id);
    return result;
  }

  std::vector<std::uint64_t> matched(portCount, 0);
  std::vector<bool> occupied(portCount, false);
  std::function<bool(const RFPortEvent&, std::vector<bool>&)> augment =
      [&](const RFPortEvent& event, std::vector<bool>& visited) {
        for (unsigned port = 0; port < portCount; ++port) {
          if (visited[port] || !accepts(bank, event, port))
            continue;
          visited[port] = true;
          if (!occupied[port]) {
            occupied[port] = true;
            matched[port] = event.id;
            return true;
          }
          const auto previous = std::ranges::find_if(
              ordered, [&](const auto* candidate) { return candidate->id == matched[port]; });
          if (previous != ordered.end() && augment(**previous, visited)) {
            matched[port] = event.id;
            return true;
          }
        }
        return false;
      };

  for (const auto* event : ordered) {
    std::vector<bool> visited(portCount, false);
    if (!augment(*event, visited)) {
      result.status = writes ? RFPortMatchStatus::SourceCompatibilityFailure
                             : RFPortMatchStatus::CapacityExceeded;
      result.conflictingEvents.push_back(event->id);
      return result;
    }
  }
  for (unsigned port = 0; port < portCount; ++port)
    if (occupied[port])
      result.assignments.push_back({matched[port], port});
  std::ranges::sort(result.assignments,
                    [](const auto& lhs, const auto& rhs) { return lhs.eventId < rhs.eventId; });
  return result;
}

} // namespace cgra::register_allocation
