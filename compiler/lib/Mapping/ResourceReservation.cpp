// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ResourceReservation.h"

#include <algorithm>
#include <stdexcept>

namespace cgra::mapping {

ResourceReservationTable::ResourceReservationTable(const ModuloResourceModel& model)
    : owners_(model.resourceCount()) {}

bool ResourceReservationTable::isFree(ResourceId resource) const {
  if (resource >= owners_.size())
    throw std::out_of_range("unknown reservation resource id");
  return !owners_[resource].has_value();
}

std::optional<ReservationOwner> ResourceReservationTable::owner(ResourceId resource) const {
  if (resource >= owners_.size())
    throw std::out_of_range("unknown reservation resource id");
  return owners_[resource];
}

bool ResourceReservationTable::canReserve(std::span<const ResourceId> resources) const {
  std::vector<ResourceId> unique;
  unique.reserve(resources.size());
  for (const auto resource : resources) {
    if (resource >= owners_.size() ||
        std::find(unique.begin(), unique.end(), resource) != unique.end() ||
        owners_[resource].has_value())
      return false;
    unique.push_back(resource);
  }
  return true;
}

bool ResourceReservationTable::reserve(std::span<const ResourceId> resources,
                                       ReservationOwner reservationOwner) {
  if (!canReserve(resources))
    return false;
  for (const auto resource : resources)
    owners_[resource] = reservationOwner;
  return true;
}

std::optional<ReservationDelta>
ResourceReservationTable::tryReserve(std::span<const ResourceId> resources,
                                     ReservationOwner reservationOwner) {
  if (!reserve(resources, reservationOwner))
    return std::nullopt;
  return ReservationDelta{reservationOwner, {resources.begin(), resources.end()}};
}

void ResourceReservationTable::release(std::span<const ResourceId> resources,
                                       ReservationOwner reservationOwner) {
  for (const auto resource : resources) {
    if (resource >= owners_.size() || !owners_[resource] || *owners_[resource] != reservationOwner)
      throw std::invalid_argument("reservation release owner mismatch");
  }
  for (const auto resource : resources)
    owners_[resource].reset();
}

void ResourceReservationTable::undo(const ReservationDelta& delta) {
  release(delta.resources, delta.owner);
}

void ResourceReservationTable::clear() noexcept {
  for (auto& owner : owners_)
    owner.reset();
}

} // namespace cgra::mapping
