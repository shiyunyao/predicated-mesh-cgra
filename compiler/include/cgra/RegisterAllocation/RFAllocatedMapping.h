// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/StorageRequirement.h"
#include "cgra/Schedule/StagedMapping.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cgra::register_allocation {

struct PhysicalRegister {
  cgra::mapping::TileCoord tile;
  cgra::RegisterBankId bank;
  std::uint32_t index = 0;

  friend bool operator==(const PhysicalRegister&, const PhysicalRegister&) = default;
};

struct PhaseRegisterAllocation {
  std::uint32_t phase = 0;
  PhysicalRegister reg;

  friend bool operator==(const PhaseRegisterAllocation&, const PhaseRegisterAllocation&) =
      default;
};

struct PeriodicRegisterFamily {
  StorageSegmentId segment = 0;
  std::uint32_t phaseCount = 1;
  std::vector<PhaseRegisterAllocation> phases;

  friend bool operator==(const PeriodicRegisterFamily&, const PeriodicRegisterFamily&) = default;
};

struct BoundaryWriteAllocation {
  std::int64_t producerIteration = 0;
  std::uint32_t writePort = 0;
  friend bool operator==(const BoundaryWriteAllocation&, const BoundaryWriteAllocation&) = default;
};

struct StorageAllocation {
  StorageSegmentId segment = 0;
  PhysicalRegister reg;
  // Exact target access ports selected by T010. They are part of the
  // allocation, rather than a lowering-time convention.
  std::uint32_t readPort = 0;
  std::uint32_t writePort = 0;
  // A recurrence boundary may enter the same allocated register through a
  // different target source than the steady-state producer.  When present,
  // this is the exact port assigned for that finite boundary write.
  std::optional<std::uint32_t> boundaryWritePort;
  PeriodicRegisterFamily family;
  std::vector<BoundaryWriteAllocation> boundaryWrites;

  std::optional<std::uint32_t> boundaryWriteFor(std::int64_t producerIteration) const {
    for (const auto& boundary : boundaryWrites)
      if (boundary.producerIteration == producerIteration)
        return boundary.writePort;
    return boundaryWritePort;
  }

  friend bool operator==(const StorageAllocation&, const StorageAllocation&) = default;
};

class RFAllocatedMapping {
public:
  const cgra::schedule::StagedMapping& staged() const noexcept { return staged_; }
  const StorageRequirements& storageRequirements() const noexcept { return requirements_; }
  std::span<const StorageAllocation> allocations() const noexcept { return allocations_; }
  PhysicalRegister registerFor(StorageSegmentId segment) const;
  PhysicalRegister registerFor(StorageSegmentId segment, std::int64_t logicalIteration) const;
  std::uint32_t rotationPeriodIterations() const;
  std::uint32_t controlPeriodCycles(std::uint32_t logicalII) const;
  const StorageAllocation& allocationFor(StorageSegmentId segment) const;
  std::optional<PhysicalRegister> registerForVirtualHold(cgra::target::TargetEdgeId edge,
                                                         std::uint32_t transportActionIndex) const;
  std::optional<PhysicalRegister> registerForTerminalSlack(cgra::target::TargetEdgeId edge) const;
  bool operator==(const RFAllocatedMapping&) const noexcept;

private:
  friend class RFAllocator;
  friend class RFAllocationVerifier;
  friend class RFAllocatedMappingSerialization;
  friend class RFAllocationTestAccess;
  RFAllocatedMapping(cgra::schedule::StagedMapping staged, StorageRequirements requirements,
                     std::vector<StorageAllocation> allocations);

  cgra::schedule::StagedMapping staged_;
  StorageRequirements requirements_;
  std::vector<StorageAllocation> allocations_;
};

} // namespace cgra::register_allocation
