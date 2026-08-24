// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloResourceModel.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace cgra::mapping {

std::string toString(TileCoord tile) {
  return "(" + std::to_string(tile.row) + "," + std::to_string(tile.col) + ")";
}

ModuloSlot ModuloTimeDomain::normalize(std::uint64_t logicalCycle) const noexcept {
  return ModuloSlot(static_cast<std::uint32_t>(logicalCycle % ii_));
}

ModuloTimeDomain::ModuloTimeDomain(std::uint32_t ii) : ii_(ii) {
  if (ii == 0)
    throw std::invalid_argument("modulo II must be at least one");
}

ModuloSlot ModuloTimeDomain::advance(ModuloSlot slot, std::uint64_t deltaCycles) const {
  validate(slot);
  const auto reducedDelta = deltaCycles % ii_;
  return normalize(static_cast<std::uint64_t>(slot.value()) + reducedDelta);
}

void ModuloTimeDomain::validate(ModuloSlot slot) const {
  if (slot.value() >= ii_)
    throw std::invalid_argument("modulo slot is outside [0, II)");
}

Direction opposite(Direction direction) noexcept {
  switch (direction) {
  case Direction::North:
    return Direction::South;
  case Direction::South:
    return Direction::North;
  case Direction::East:
    return Direction::West;
  case Direction::West:
    return Direction::East;
  }
  return Direction::North;
}

std::optional<TileCoord> neighbor(TileCoord tile, Direction direction, std::uint32_t rows,
                                  std::uint32_t cols) noexcept {
  if (tile.row >= rows || tile.col >= cols)
    return std::nullopt;
  switch (direction) {
  case Direction::North:
    return tile.row == 0 ? std::nullopt : std::optional<TileCoord>{{tile.row - 1, tile.col}};
  case Direction::South:
    return tile.row + 1 >= rows ? std::nullopt : std::optional<TileCoord>{{tile.row + 1, tile.col}};
  case Direction::East:
    return tile.col + 1 >= cols ? std::nullopt : std::optional<TileCoord>{{tile.row, tile.col + 1}};
  case Direction::West:
    return tile.col == 0 ? std::nullopt : std::optional<TileCoord>{{tile.row, tile.col - 1}};
  }
  return std::nullopt;
}

std::optional<TileCoord> neighbor(TileCoord tile, Direction direction,
                                  const cgra::TargetModel& target) noexcept {
  return neighbor(tile, direction, target.array().rows, target.array().cols);
}

ResourceKind kindOf(const ModuloResource& resource) noexcept {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FUResource>)
          return ResourceKind::FU;
        else if constexpr (std::is_same_v<T, LSUResource>)
          return ResourceKind::LSU;
        else
          return value.domain == NetworkDomain::Data ? ResourceKind::DataLink
                                                     : ResourceKind::PredicateLink;
      },
      resource);
}

std::size_t
ModuloResourceModel::ResourceKeyHash::operator()(const ResourceKey& key) const noexcept {
  std::size_t hash = static_cast<std::size_t>(key.kind);
  hash = hash * 31 + static_cast<std::size_t>(key.domain);
  hash = hash * 31 + key.tile.row;
  hash = hash * 31 + key.tile.col;
  hash = hash * 31 + static_cast<std::size_t>(key.direction);
  hash = hash * 31 + key.slot.value();
  return hash;
}

ModuloResourceModel::ResourceKey
ModuloResourceModel::keyFor(const ModuloResource& resource) noexcept {
  return std::visit(
      [](const auto& value) -> ResourceKey {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FUResource>)
          return {ResourceKind::FU, NetworkDomain::Data, value.tile, Direction::North, value.slot};
        else if constexpr (std::is_same_v<T, LSUResource>)
          return {ResourceKind::LSU, NetworkDomain::Data, value.tile, Direction::North, value.slot};
        else
          return {kindOf(ModuloResource{value}), value.domain, value.source, value.direction,
                  value.slot};
      },
      resource);
}

ModuloResourceModel::ModuloResourceModel(const cgra::TargetModel& target, std::uint32_t ii)
    : target_(&target), time_(ii) {
  stats_.ii = ii;
  for (std::uint32_t slot = 0; slot < ii; ++slot) {
    for (std::uint32_t row = 0; row < target.array().rows; ++row) {
      for (std::uint32_t col = 0; col < target.array().cols; ++col) {
        const TileCoord tile{row, col};
        addResource(FUResource{tile, ModuloSlot(slot)});
        if (target.tileHasLSU(row, col))
          addResource(LSUResource{tile, ModuloSlot(slot)});
        const bool meshLinks = target.dataNetwork().topology == "mesh_2d";
        if (meshLinks) {
          for (const auto direction :
               {Direction::North, Direction::South, Direction::East, Direction::West}) {
            if (neighbor(tile, direction, target.array().rows, target.array().cols))
              addResource(LinkResource{NetworkDomain::Data, tile, direction, ModuloSlot(slot)});
          }
          for (const auto direction :
               {Direction::North, Direction::South, Direction::East, Direction::West}) {
            if (neighbor(tile, direction, target.array().rows, target.array().cols))
              addResource(
                  LinkResource{NetworkDomain::Predicate, tile, direction, ModuloSlot(slot)});
          }
        }
      }
    }
  }
  stats_.totalResources = resources_.size();
}

ResourceId ModuloResourceModel::addResource(ModuloResource resource) {
  const auto id = static_cast<ResourceId>(resources_.size());
  switch (kindOf(resource)) {
  case ResourceKind::FU:
    ++stats_.fuResources;
    break;
  case ResourceKind::LSU:
    ++stats_.lsuResources;
    break;
  case ResourceKind::DataLink:
    ++stats_.dataLinkResources;
    break;
  case ResourceKind::PredicateLink:
    ++stats_.predicateLinkResources;
    break;
  }
  resources_.push_back(std::move(resource));
  resourceIds_.emplace(keyFor(resources_.back()), id);
  return id;
}

const ModuloResource& ModuloResourceModel::resource(ResourceId id) const {
  if (id >= resources_.size())
    throw std::out_of_range("unknown modulo resource id");
  return resources_[id];
}

ResourceId ModuloResourceModel::findResource(const ModuloResource& requested) const {
  const auto found = resourceIds_.find(keyFor(requested));
  if (found == resourceIds_.end())
    throw std::out_of_range("requested modulo resource does not exist");
  return found->second;
}

bool ModuloResourceModel::hasFU(TileCoord tile) const noexcept {
  return tile.row < target_->array().rows && tile.col < target_->array().cols;
}

bool ModuloResourceModel::hasLSU(TileCoord tile) const noexcept {
  return target_->tileHasLSU(tile.row, tile.col);
}

bool ModuloResourceModel::supportsOperation(TileCoord tile,
                                            const cgra::target::TargetNode& node) const {
  if (tile.row >= target_->array().rows || tile.col >= target_->array().cols)
    return false;
  const auto* operation = target_->findOperation(node.operation);
  if (!operation)
    return false;
  return target_->tileSupportsOperation(tile.row, tile.col, operation->id);
}

ResourceId ModuloResourceModel::fuResource(TileCoord tile, ModuloSlot slot) const {
  time_.validate(slot);
  if (!hasFU(tile))
    throw std::invalid_argument("tile is outside the target array");
  return findResource(FUResource{tile, slot});
}

std::optional<ResourceId> ModuloResourceModel::lsuResource(TileCoord tile, ModuloSlot slot) const {
  time_.validate(slot);
  if (!hasLSU(tile))
    return std::nullopt;
  return findResource(LSUResource{tile, slot});
}

std::optional<ResourceId> ModuloResourceModel::linkResource(NetworkDomain domain, TileCoord source,
                                                            Direction direction,
                                                            ModuloSlot slot) const {
  time_.validate(slot);
  if (!neighbor(source, direction, target_->array().rows, target_->array().cols))
    return std::nullopt;
  try {
    return findResource(LinkResource{domain, source, direction, slot});
  } catch (const std::out_of_range&) {
    // A target contract may intentionally describe a disconnected topology;
    // absent links are a normal no-route result, not a malformed ResourceId.
    return std::nullopt;
  }
}

std::vector<ResourceId>
ModuloResourceModel::operationFootprint(const cgra::target::TargetNode& node, TileCoord tile,
                                        ModuloSlot issueSlot) const {
  const auto* operation = target_->findOperation(node.operation);
  if (!operation)
    throw std::invalid_argument("operation is absent from TargetModel");
  if (!supportsOperation(tile, node))
    throw std::invalid_argument("operation cannot execute on requested tile");
  if (operation->issueOccupancy == 0)
    throw std::invalid_argument("operation issue occupancy must be positive");
  std::vector<ResourceId> footprint;
  footprint.reserve(operation->issueOccupancy);
  for (unsigned offset = 0; offset < operation->issueOccupancy; ++offset) {
    const auto slot = time_.advance(issueSlot, offset);
    const auto resource = operation->executionClass == cgra::TargetExecutionClass::FU
                              ? fuResource(tile, slot)
                              : lsuResource(tile, slot).value();
    if (std::find(footprint.begin(), footprint.end(), resource) != footprint.end())
      throw std::invalid_argument("operation footprint overlaps itself in modulo time");
    footprint.push_back(resource);
  }
  return footprint;
}

} // namespace cgra::mapping
