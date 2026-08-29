// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/MaterializedEvent.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cgra::schedule {

struct CycleBundle {
  std::vector<MaterializedEvent> events;
  friend bool operator==(const CycleBundle&, const CycleBundle&) = default;
};

struct SchedulePhase {
  std::vector<CycleBundle> cycles;
  friend bool operator==(const SchedulePhase&, const SchedulePhase&) = default;
};

struct RepeatingKernel {
  std::vector<CycleBundle> body;
  std::uint64_t repeatCount = 0;
  friend bool operator==(const RepeatingKernel&, const RepeatingKernel&) = default;
};

class MaterializedSchedule {
public:
  std::uint32_t ii() const noexcept { return ii_; }
  std::uint32_t logicalII() const noexcept { return ii_; }
  std::uint32_t rotationPeriodIterations() const noexcept { return rotationPeriodIterations_; }
  std::uint32_t controlPeriodCycles() const noexcept { return controlPeriodCycles_; }
  std::uint64_t tripCount() const noexcept { return tripCount_; }
  std::uint64_t timeOriginShift() const noexcept { return timeOriginShift_; }
  std::uint64_t totalLogicalCycles() const noexcept { return totalLogicalCycles_; }
  const SchedulePhase& prologue() const noexcept { return prologue_; }
  const RepeatingKernel& kernel() const noexcept { return kernel_; }
  const SchedulePhase& epilogue() const noexcept { return epilogue_; }

  friend bool operator==(const MaterializedSchedule&, const MaterializedSchedule&) = default;

private:
  friend class ScheduleMaterializer;
  friend class MaterializedScheduleVerifier;
  friend class MaterializedScheduleSerialization;
  friend class MaterializedScheduleTestAccess;
  MaterializedSchedule(std::uint32_t ii, std::uint64_t tripCount, std::uint64_t timeOriginShift,
                       std::uint64_t totalLogicalCycles, SchedulePhase prologue,
                       RepeatingKernel kernel, SchedulePhase epilogue)
      : ii_(ii), tripCount_(tripCount), timeOriginShift_(timeOriginShift),
        totalLogicalCycles_(totalLogicalCycles), prologue_(std::move(prologue)),
        kernel_(std::move(kernel)), epilogue_(std::move(epilogue)),
        rotationPeriodIterations_(1), controlPeriodCycles_(ii) {}
  MaterializedSchedule(std::uint32_t ii, std::uint64_t tripCount, std::uint64_t timeOriginShift,
                       std::uint64_t totalLogicalCycles, std::uint32_t rotationPeriodIterations,
                       std::uint32_t controlPeriodCycles, SchedulePhase prologue,
                       RepeatingKernel kernel, SchedulePhase epilogue)
      : ii_(ii), tripCount_(tripCount), timeOriginShift_(timeOriginShift),
        totalLogicalCycles_(totalLogicalCycles), prologue_(std::move(prologue)),
        kernel_(std::move(kernel)), epilogue_(std::move(epilogue)),
        rotationPeriodIterations_(rotationPeriodIterations), controlPeriodCycles_(controlPeriodCycles) {}

  std::uint32_t ii_ = 0;
  std::uint64_t tripCount_ = 0;
  std::uint64_t timeOriginShift_ = 0;
  std::uint64_t totalLogicalCycles_ = 0;
  SchedulePhase prologue_;
  RepeatingKernel kernel_;
  SchedulePhase epilogue_;
  std::uint32_t rotationPeriodIterations_ = 1;
  std::uint32_t controlPeriodCycles_ = 0;
};

} // namespace cgra::schedule
