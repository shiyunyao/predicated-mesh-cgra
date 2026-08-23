// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/ValueType.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cgra {

using TargetOperationRef = std::string;

enum class TargetExecutionClass {
  FU,
  LSU,
};

std::string_view toString(TargetExecutionClass executionClass);
TargetExecutionClass targetExecutionClassFromString(std::string_view value);

struct TargetOperationDesc {
  TargetOperationRef id;
  TargetExecutionClass executionClass = TargetExecutionClass::FU;
  ir::ValueType resultType = ir::ValueType::voidTy();
  unsigned issueOccupancy = 1;
  std::optional<unsigned> resultLatency;
  std::optional<unsigned> producerOutputReadyOffset;
  std::optional<unsigned> accessWidthBits;

  friend bool operator==(const TargetOperationDesc&, const TargetOperationDesc&) = default;
};

} // namespace cgra
