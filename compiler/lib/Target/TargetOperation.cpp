// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetOperation.h"

#include <stdexcept>

namespace cgra {

std::string_view toString(TargetExecutionClass executionClass) {
  switch (executionClass) {
  case TargetExecutionClass::FU:
    return "FU";
  case TargetExecutionClass::LSU:
    return "LSU";
  }
  return "UNKNOWN";
}

TargetExecutionClass targetExecutionClassFromString(std::string_view value) {
  if (value == "FU")
    return TargetExecutionClass::FU;
  if (value == "LSU")
    return TargetExecutionClass::LSU;
  throw std::invalid_argument("unknown target execution class: " + std::string(value));
}

} // namespace cgra
