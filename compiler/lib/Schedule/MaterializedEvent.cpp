// SPDX-License-Identifier: MIT
#include "cgra/Schedule/MaterializedEvent.h"

#include <stdexcept>

namespace cgra::schedule {

std::string_view toString(MaterializedEventKind kind) noexcept {
  switch (kind) {
  case MaterializedEventKind::BoundaryValueInject:
    return "boundary_value_inject";
  case MaterializedEventKind::RFRead:
    return "rf_read";
  case MaterializedEventKind::NodeIssue:
    return "node_issue";
  case MaterializedEventKind::LinkLaunch:
    return "link_launch";
  case MaterializedEventKind::RFWrite:
    return "rf_write";
  case MaterializedEventKind::LiveOutBoundaryUse:
    return "live_out_boundary_use";
  }
  return "unknown";
}

MaterializedEventKind materializedEventKindFromString(std::string_view value) {
  if (value == "boundary_value_inject")
    return MaterializedEventKind::BoundaryValueInject;
  if (value == "rf_read")
    return MaterializedEventKind::RFRead;
  if (value == "node_issue")
    return MaterializedEventKind::NodeIssue;
  if (value == "link_launch")
    return MaterializedEventKind::LinkLaunch;
  if (value == "rf_write")
    return MaterializedEventKind::RFWrite;
  if (value == "live_out_boundary_use")
    return MaterializedEventKind::LiveOutBoundaryUse;
  throw std::invalid_argument("unknown materialized event kind");
}

} // namespace cgra::schedule
