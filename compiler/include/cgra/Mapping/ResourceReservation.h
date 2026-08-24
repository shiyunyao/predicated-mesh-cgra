// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloResourceModel.h"

#include <optional>
#include <span>
#include <vector>

namespace cgra::mapping {

enum class ReservationOwnerKind {
  Node,
  Edge,
};

struct ReservationOwner {
  ReservationOwnerKind kind = ReservationOwnerKind::Node;
  std::uint32_t id = 0;
  friend bool operator==(const ReservationOwner&, const ReservationOwner&) = default;
};

struct ReservationDelta {
  ReservationOwner owner;
  std::vector<ResourceId> resources;
};

class ResourceReservationTable {
public:
  explicit ResourceReservationTable(const ModuloResourceModel& model);

  bool isFree(ResourceId resource) const;
  std::optional<ReservationOwner> owner(ResourceId resource) const;
  bool canReserve(std::span<const ResourceId> resources) const;
  bool reserve(std::span<const ResourceId> resources, ReservationOwner owner);
  std::optional<ReservationDelta> tryReserve(std::span<const ResourceId> resources,
                                             ReservationOwner owner);
  void release(std::span<const ResourceId> resources, ReservationOwner owner);
  void undo(const ReservationDelta& delta);
  void clear() noexcept;

private:
  std::vector<std::optional<ReservationOwner>> owners_;
};

} // namespace cgra::mapping
