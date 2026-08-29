// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/ControlLayout.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cgra::lowering {

struct TargetControlCycle {
  std::vector<TileControl> tiles;
  friend bool operator==(const TargetControlCycle&, const TargetControlCycle&) = default;
};

struct TargetControlPhase {
  std::vector<TargetControlCycle> cycles;
  friend bool operator==(const TargetControlPhase&, const TargetControlPhase&) = default;
};

struct RepeatingTargetKernel {
  std::vector<TargetControlCycle> body;
  std::uint64_t repeatCount = 0;
  friend bool operator==(const RepeatingTargetKernel&, const RepeatingTargetKernel&) = default;
};

class TargetControlProgram {
public:
  std::uint32_t ii() const noexcept { return ii_; }
  std::uint32_t logicalII() const noexcept { return ii_; }
  std::uint32_t rotationPeriodIterations() const noexcept { return rotationPeriodIterations_; }
  std::uint32_t controlPeriodCycles() const noexcept { return controlPeriodCycles_; }
  std::uint64_t tripCount() const noexcept { return tripCount_; }
  const TargetControlPhase& prologue() const noexcept { return prologue_; }
  const RepeatingTargetKernel& kernel() const noexcept { return kernel_; }
  const TargetControlPhase& epilogue() const noexcept { return epilogue_; }

  friend bool operator==(const TargetControlProgram&, const TargetControlProgram&) = default;
  TargetControlProgram(std::uint32_t ii, std::uint64_t tripCount, TargetControlPhase prologue,
                       RepeatingTargetKernel kernel, TargetControlPhase epilogue)
      : ii_(ii), tripCount_(tripCount), prologue_(std::move(prologue)), kernel_(std::move(kernel)),
        epilogue_(std::move(epilogue)), rotationPeriodIterations_(1), controlPeriodCycles_(ii) {}
  TargetControlProgram(std::uint32_t ii, std::uint64_t tripCount,
                       std::uint32_t rotationPeriodIterations, std::uint32_t controlPeriodCycles,
                       TargetControlPhase prologue, RepeatingTargetKernel kernel,
                       TargetControlPhase epilogue)
      : ii_(ii), tripCount_(tripCount), prologue_(std::move(prologue)), kernel_(std::move(kernel)),
        epilogue_(std::move(epilogue)), rotationPeriodIterations_(rotationPeriodIterations),
        controlPeriodCycles_(controlPeriodCycles) {}

private:
  std::uint32_t ii_ = 0;
  std::uint64_t tripCount_ = 0;
  TargetControlPhase prologue_;
  RepeatingTargetKernel kernel_;
  TargetControlPhase epilogue_;
  std::uint32_t rotationPeriodIterations_ = 1;
  std::uint32_t controlPeriodCycles_ = 0;
};

} // namespace cgra::lowering
