// SPDX-License-Identifier: MIT
#include "cgra/Lowering/ConstantAllocator.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <stdexcept>

namespace cgra::lowering {

std::optional<PhysicalConstantLocation> ConstantImage::location(ir::ConstantId id) const noexcept {
  const auto it =
      std::ranges::find_if(entries, [id](const auto& entry) { return entry.constant == id; });
  if (it == entries.end())
    return std::nullopt;
  return it->location;
}

std::uint32_t ConstantImage::address(ir::ConstantId id) const {
  const auto found = location(id);
  if (!found)
    throw std::out_of_range("constant has no physical allocation");
  return found->address;
}

ConstantImage ConstantAllocator::allocate(const target::TargetDFG& dfg, const TargetModel& target) {
  ConstantImage image;
  std::map<std::uint32_t, std::uint32_t> valueAddresses;
  for (const auto& constant : dfg.constants()) {
    auto [it, inserted] =
        valueAddresses.emplace(constant.bits, static_cast<std::uint32_t>(valueAddresses.size()));
    if (inserted && it->second >= target.constantMemoryDepth())
      throw std::runtime_error("constant memory capacity exceeded");
    image.entries.push_back({constant.id, {it->second}, constant.bits});
  }
  std::ranges::sort(image.entries,
                    [](const auto& lhs, const auto& rhs) { return lhs.constant < rhs.constant; });
  return image;
}

} // namespace cgra::lowering
