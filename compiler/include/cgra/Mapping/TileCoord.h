// SPDX-License-Identifier: MIT
#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace cgra::mapping {

struct TileCoord {
  std::uint32_t row = 0;
  std::uint32_t col = 0;

  friend bool operator==(const TileCoord&, const TileCoord&) = default;
  friend auto operator<=>(const TileCoord&, const TileCoord&) = default;
};

std::string toString(TileCoord tile);

} // namespace cgra::mapping
