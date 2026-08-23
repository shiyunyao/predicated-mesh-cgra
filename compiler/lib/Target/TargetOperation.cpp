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

std::string_view toString(TargetOperandRole role) {
  switch (role) {
  case TargetOperandRole::Data:
    return "data";
  case TargetOperandRole::Predicate:
    return "predicate";
  case TargetOperandRole::Address:
    return "address";
  }
  return "unknown";
}

TargetOperandRole targetOperandRoleFromString(std::string_view value) {
  if (value == "data")
    return TargetOperandRole::Data;
  if (value == "predicate")
    return TargetOperandRole::Predicate;
  if (value == "address")
    return TargetOperandRole::Address;
  throw std::invalid_argument("unknown target operand role: " + std::string(value));
}

std::string_view toString(TargetResultRole role) {
  switch (role) {
  case TargetResultRole::Data:
    return "data";
  case TargetResultRole::Predicate:
    return "predicate";
  case TargetResultRole::Void:
    return "void";
  }
  return "unknown";
}

TargetResultRole targetResultRoleFromString(std::string_view value) {
  if (value == "data")
    return TargetResultRole::Data;
  if (value == "predicate")
    return TargetResultRole::Predicate;
  if (value == "void")
    return TargetResultRole::Void;
  throw std::invalid_argument("unknown target result role: " + std::string(value));
}

} // namespace cgra
