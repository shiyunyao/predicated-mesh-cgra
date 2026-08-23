// SPDX-License-Identifier: MIT
#pragma once

#include <compare>
#include <cstdint>

namespace cgra::mapping {

class ModuloSlot {
public:
  explicit constexpr ModuloSlot(std::uint32_t value = 0) : value_(value) {}

  constexpr std::uint32_t value() const noexcept { return value_; }
  friend constexpr bool operator==(ModuloSlot, ModuloSlot) = default;
  friend constexpr auto operator<=>(ModuloSlot, ModuloSlot) = default;

private:
  std::uint32_t value_;
};

class ModuloTimeDomain {
public:
  explicit ModuloTimeDomain(std::uint32_t ii);

  std::uint32_t ii() const noexcept { return ii_; }
  ModuloSlot normalize(std::uint64_t logicalCycle) const noexcept;
  ModuloSlot advance(ModuloSlot slot, std::uint64_t deltaCycles) const;
  void validate(ModuloSlot slot) const;

private:
  std::uint32_t ii_;
};

} // namespace cgra::mapping
