// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloTime.h"
#include "cgra/Mapping/Network.h"

#include <cstdint>
#include <variant>

namespace cgra::mapping {

using ResourceId = std::uint32_t;

enum class ResourceKind {
  FU,
  LSU,
  DataLink,
  PredicateLink,
};

struct FUResource {
  TileCoord tile;
  ModuloSlot slot;
  friend bool operator==(const FUResource&, const FUResource&) = default;
};

struct LSUResource {
  TileCoord tile;
  ModuloSlot slot;
  friend bool operator==(const LSUResource&, const LSUResource&) = default;
};

struct LinkResource {
  NetworkDomain domain = NetworkDomain::Data;
  TileCoord source;
  Direction direction = Direction::North;
  ModuloSlot slot;
  friend bool operator==(const LinkResource&, const LinkResource&) = default;
};

using ModuloResource = std::variant<FUResource, LSUResource, LinkResource>;

ResourceKind kindOf(const ModuloResource& resource) noexcept;

} // namespace cgra::mapping
