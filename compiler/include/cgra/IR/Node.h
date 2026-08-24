// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/Opcode.h"
#include "cgra/IR/ValueType.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cgra::ir {

using NodeId = std::uint32_t;
using ExternalValueId = std::uint32_t;
using ConstantId = std::uint32_t;
using LiveOutId = std::uint32_t;

struct SourceInfo {
  std::string label;

  friend bool operator==(const SourceInfo&, const SourceInfo&) = default;
};

struct MemoryOpInfo {
  std::uint32_t accessWidthBits = 0;
  bool isVolatile = false;

  friend bool operator==(const MemoryOpInfo&, const MemoryOpInfo&) = default;
};

struct Node {
  NodeId id = 0;
  Opcode opcode = Opcode::Add;
  ValueType resultType = ValueType::voidTy();
  std::vector<ValueType> operandTypes;
  std::optional<ICmpPredicate> icmpPredicate;
  std::optional<MemoryOpInfo> memoryInfo;
  std::optional<SourceInfo> source;

  friend bool operator==(const Node&, const Node&) = default;
};

struct ExternalValue {
  ExternalValueId id = 0;
  ValueType type = ValueType::voidTy();
  std::string name;

  friend bool operator==(const ExternalValue&, const ExternalValue&) = default;
};

struct ConstantValue {
  ConstantId id = 0;
  ValueType type = ValueType::voidTy();
  std::uint64_t bits = 0;

  friend bool operator==(const ConstantValue&, const ConstantValue&) = default;
};

struct LiveOut {
  LiveOutId id = 0;
  ValueType type = ValueType::voidTy();
  std::string name;
  NodeId source = 0;

  friend bool operator==(const LiveOut&, const LiveOut&) = default;
};

struct ExternalValueRef {
  ExternalValueId value = 0;

  friend bool operator==(const ExternalValueRef&, const ExternalValueRef&) = default;
};

struct ConstantRef {
  ConstantId value = 0;

  friend bool operator==(const ConstantRef&, const ConstantRef&) = default;
};

using ExternalOperandBinding = std::variant<ExternalValueRef, ConstantRef>;

struct OperandBinding {
  NodeId node = 0;
  std::uint32_t operand = 0;
  ExternalOperandBinding source;

  friend bool operator==(const OperandBinding&, const OperandBinding&) = default;
};

} // namespace cgra::ir
