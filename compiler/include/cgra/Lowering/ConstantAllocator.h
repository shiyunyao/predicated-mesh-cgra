// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace cgra::lowering {

struct PhysicalConstantLocation {
  std::uint32_t address = 0;
  friend bool operator==(const PhysicalConstantLocation&,
                         const PhysicalConstantLocation&) = default;
};

struct ConstantAllocation {
  ir::ConstantId constant = 0;
  PhysicalConstantLocation location;
  std::uint64_t bits = 0;
  friend bool operator==(const ConstantAllocation&, const ConstantAllocation&) = default;
};

struct ConstantImage {
  std::vector<ConstantAllocation> entries;
  std::optional<PhysicalConstantLocation> location(ir::ConstantId id) const noexcept;
  std::uint32_t address(ir::ConstantId id) const;
};

class ConstantAllocator {
public:
  static ConstantImage allocate(const target::TargetDFG& dfg, const TargetModel& target);
};

} // namespace cgra::lowering
