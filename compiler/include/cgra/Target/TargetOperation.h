// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/ValueType.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra {

using TargetOperationRef = std::string;

enum class TargetExecutionClass {
  FU,
  LSU,
};

enum class TargetOperandRole {
  Data,
  Predicate,
  Address,
};

enum class TargetResultRole {
  Data,
  Predicate,
  Void,
};

struct TargetOperandDesc {
  TargetOperandRole role = TargetOperandRole::Data;
  bool optional = false;

  friend bool operator==(const TargetOperandDesc&, const TargetOperandDesc&) = default;
};

std::string_view toString(TargetExecutionClass executionClass);
TargetExecutionClass targetExecutionClassFromString(std::string_view value);
std::string_view toString(TargetOperandRole role);
TargetOperandRole targetOperandRoleFromString(std::string_view value);
std::string_view toString(TargetResultRole role);
TargetResultRole targetResultRoleFromString(std::string_view value);

struct TargetOperationDesc {
  TargetOperationRef id;
  TargetExecutionClass executionClass = TargetExecutionClass::FU;
  std::vector<TargetOperandDesc> operands;
  TargetResultRole resultRole = TargetResultRole::Void;
  ir::ValueType resultType = ir::ValueType::voidTy();
  unsigned issueOccupancy = 1;
  std::optional<unsigned> resultLatency;
  std::optional<unsigned> producerOutputReadyOffset;
  std::optional<unsigned> accessWidthBits;

  friend bool operator==(const TargetOperationDesc&, const TargetOperationDesc&) = default;
};

} // namespace cgra
