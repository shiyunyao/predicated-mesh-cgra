// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"

#include <cstdint>
#include <optional>

namespace cgra::lowering {

enum class ValueSourceKind {
  FuDataResult,
  FuPredicateResult,
  LsuLoadData,
  DataRF,
  PredicateRF,
  NorthDataIn,
  SouthDataIn,
  EastDataIn,
  WestDataIn,
  NorthPredicateIn,
  SouthPredicateIn,
  EastPredicateIn,
  WestPredicateIn,
  ConstantData,
  ConstantTrue,
  ConstantFalse,
  Zero,
};

struct ResolvedValueSource {
  ValueSourceKind kind = ValueSourceKind::Zero;
  std::optional<register_allocation::PhysicalRegister> reg;
  std::optional<std::uint32_t> rfPort;
  std::optional<std::uint32_t> constantAddress;
  friend bool operator==(const ResolvedValueSource&, const ResolvedValueSource&) = default;
};

} // namespace cgra::lowering
