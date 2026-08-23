// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/TileCoord.h"

#include <optional>

namespace cgra {
class TargetModel;
}

namespace cgra::mapping {

enum class Direction {
  North,
  South,
  East,
  West,
};

enum class NetworkDomain {
  Data,
  Predicate,
};

Direction opposite(Direction direction) noexcept;
std::optional<TileCoord> neighbor(TileCoord tile, Direction direction, std::uint32_t rows,
                                  std::uint32_t cols) noexcept;
std::optional<TileCoord> neighbor(TileCoord tile, Direction direction,
                                  const cgra::TargetModel& target) noexcept;

} // namespace cgra::mapping
