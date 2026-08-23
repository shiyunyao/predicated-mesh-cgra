// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloResource.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cgra::mapping {

struct ModuloResourceStats {
  std::uint32_t ii = 0;
  std::uint64_t fuResources = 0;
  std::uint64_t lsuResources = 0;
  std::uint64_t dataLinkResources = 0;
  std::uint64_t predicateLinkResources = 0;
  std::uint64_t totalResources = 0;
};

class ModuloResourceModel {
public:
  ModuloResourceModel(const cgra::TargetModel& target, std::uint32_t ii);

  std::uint32_t ii() const noexcept { return time_.ii(); }
  const cgra::TargetModel& target() const noexcept { return *target_; }
  std::size_t resourceCount() const noexcept { return resources_.size(); }
  const ModuloResource& resource(ResourceId id) const;
  std::span<const ModuloResource> resources() const noexcept { return resources_; }
  ModuloResourceStats stats() const noexcept { return stats_; }

  bool hasFU(TileCoord tile) const noexcept;
  bool hasLSU(TileCoord tile) const noexcept;
  bool supportsOperation(TileCoord tile, const cgra::target::TargetNode& node) const;

  ResourceId fuResource(TileCoord tile, ModuloSlot slot) const;
  std::optional<ResourceId> lsuResource(TileCoord tile, ModuloSlot slot) const;
  std::optional<ResourceId> linkResource(NetworkDomain domain, TileCoord source,
                                         Direction direction, ModuloSlot slot) const;

  std::vector<ResourceId> operationFootprint(const cgra::target::TargetNode& node, TileCoord tile,
                                             ModuloSlot issueSlot) const;

private:
  const cgra::TargetModel* target_;
  ModuloTimeDomain time_;
  std::vector<ModuloResource> resources_;
  ModuloResourceStats stats_;

  ResourceId addResource(ModuloResource resource);
  ResourceId findResource(const ModuloResource& resource) const;
};

} // namespace cgra::mapping
