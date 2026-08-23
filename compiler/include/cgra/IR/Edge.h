// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/Node.h"

#include <cstdint>
#include <string_view>
#include <variant>

namespace cgra::ir {

using EdgeId = std::uint32_t;

struct DataEdgeInfo {
  std::uint32_t dstOperand = 0;

  friend bool operator==(const DataEdgeInfo&, const DataEdgeInfo&) = default;
};

struct PredicateEdgeInfo {
  std::uint32_t dstOperand = 0;

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
