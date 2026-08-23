// SPDX-License-Identifier: MIT
#include "cgra/IR/Edge.h"

#include <stdexcept>

namespace cgra::ir {

std::string_view toString(MemoryDepKind dependence) {
  switch (dependence) {
  case MemoryDepKind::RAW:
    return "RAW";
  case MemoryDepKind::WAR:
    return "WAR";
  case MemoryDepKind::WAW:
    return "WAW";
  }
  throw std::logic_error("unknown memory dependence kind");
}

MemoryDepKind memoryDepKindFromString(std::string_view value) {
  if (value == "RAW")
    return MemoryDepKind::RAW;
  if (value == "WAR")
    return MemoryDepKind::WAR;
  if (value == "WAW")
    return MemoryDepKind::WAW;
  throw std::invalid_argument("unknown memory dependence kind: " + std::string(value));
}

} // namespace cgra::ir
