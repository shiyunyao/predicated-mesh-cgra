// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/Node.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace cgra::ir {

using EdgeId = std::uint32_t;

struct RecurrenceBoundaryValue {
  std::uint32_t iterationOffset = 0;
  ExternalOperandBinding value = ExternalValueRef{};

  friend bool operator==(const RecurrenceBoundaryValue&, const RecurrenceBoundaryValue&) = default;
};

struct RecurrenceBoundary {
  std::vector<RecurrenceBoundaryValue> values;

  friend bool operator==(const RecurrenceBoundary&, const RecurrenceBoundary&) = default;
};

struct DataEdgeInfo {
  std::uint32_t dstOperand = 0;
  std::optional<RecurrenceBoundary> boundary;

  DataEdgeInfo() = default;
  explicit DataEdgeInfo(std::uint32_t operand,
                        std::optional<RecurrenceBoundary> recurrenceBoundary = std::nullopt)
      : dstOperand(operand), boundary(std::move(recurrenceBoundary)) {}

  friend bool operator==(const DataEdgeInfo&, const DataEdgeInfo&) = default;
};

struct PredicateEdgeInfo {
  std::uint32_t dstOperand = 0;
  std::optional<RecurrenceBoundary> boundary;

  PredicateEdgeInfo() = default;
  explicit PredicateEdgeInfo(std::uint32_t operand,
                             std::optional<RecurrenceBoundary> recurrenceBoundary = std::nullopt)
      : dstOperand(operand), boundary(std::move(recurrenceBoundary)) {}

  friend bool operator==(const PredicateEdgeInfo&, const PredicateEdgeInfo&) = default;
};

enum class MemoryDepKind {
  RAW,
  WAR,
  WAW,
};

std::string_view toString(MemoryDepKind dependence);
MemoryDepKind memoryDepKindFromString(std::string_view value);

struct MemoryEdgeInfo {
  MemoryDepKind dependence = MemoryDepKind::RAW;

  friend bool operator==(const MemoryEdgeInfo&, const MemoryEdgeInfo&) = default;
};

using EdgeInfo = std::variant<DataEdgeInfo, PredicateEdgeInfo, MemoryEdgeInfo>;

struct Edge {
  EdgeId id = 0;
  NodeId src = 0;
  NodeId dst = 0;
  std::uint32_t distance = 0;
  EdgeInfo info = DataEdgeInfo{};

  enum class Kind {
    Data,
    Predicate,
    Memory,
  };

  Kind kind() const noexcept {
    switch (info.index()) {
    case 0:
      return Kind::Data;
    case 1:
      return Kind::Predicate;
    default:
      return Kind::Memory;
    }
  }

  friend bool operator==(const Edge&, const Edge&) = default;
};

} // namespace cgra::ir
