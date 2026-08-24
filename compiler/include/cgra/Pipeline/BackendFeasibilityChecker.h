// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/CompleteMappingChecker.h"
#include "cgra/RegisterAllocation/RFAllocationBudget.h"

#include <utility>

namespace cgra::pipeline {

// Runs the post-mapping stages that decide whether a complete modulo mapping
// can become an executable backend schedule.  It deliberately returns only an
// opaque mapper decision; the accepted artifacts are recomputed by the driver.
class BackendFeasibilityChecker {
public:
  explicit BackendFeasibilityChecker(register_allocation::RFAllocationOptions rfOptions = {})
      : rfOptions_(std::move(rfOptions)) {}

  mapping::CompleteMappingCheckResult check(const target::TargetDFG& dfg, const TargetModel& target,
                                            const mapping::ModuloMapping& mapping) const;

private:
  register_allocation::RFAllocationOptions rfOptions_;
};

} // namespace cgra::pipeline
