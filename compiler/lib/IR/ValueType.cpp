// SPDX-License-Identifier: MIT
#include "cgra/IR/ValueType.h"

#include <stdexcept>

namespace cgra::ir {

std::string_view toString(ValueKind kind) {
  switch (kind) {
  case ValueKind::Integer:
    return "integer";
  case ValueKind::Predicate:
    return "predicate";
  case ValueKind::Float:
    return "float";
  case ValueKind::Void:
    return "void";
  }
  throw std::logic_error("unknown value kind");
}

ValueKind valueKindFromString(std::string_view value) {
  if (value == "integer")
    return ValueKind::Integer;
  if (value == "predicate")
    return ValueKind::Predicate;
  if (value == "float")
    return ValueKind::Float;
  if (value == "void")
    return ValueKind::Void;
  throw std::invalid_argument("unknown value kind: " + std::string(value));
}

std::string ValueType::toString() const {
  if (kind == ValueKind::Void)
    return "void";
  if (kind == ValueKind::Predicate)
    return "predicate";
  const char prefix = kind == ValueKind::Integer ? 'i' : 'f';
  return std::string(1, prefix) + std::to_string(bitWidth);
}

ValueType ValueType::fromString(std::string_view value) {
  if (value == "void")
    return voidTy();
  if (value == "predicate" || value == "predicate1" || value == "i1")
    return predicate();

  const auto parseWidth = [&](std::string_view prefix, ValueKind kind) {
    if (!value.starts_with(prefix))
      return ValueType{};
    const auto digits = value.substr(prefix.size());
    if (digits.empty())
      throw std::invalid_argument("missing type width: " + std::string(value));
    unsigned width = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9')
        throw std::invalid_argument("invalid type width: " + std::string(value));
      width = width * 10U + static_cast<unsigned>(digit - '0');
    }
    if (width == 0 || width > 0xffffU)
      throw std::invalid_argument("type width out of range: " + std::string(value));
    return ValueType{kind, static_cast<std::uint16_t>(width)};
  };

  if (value.starts_with('i'))
    return parseWidth("i", ValueKind::Integer);
  if (value.starts_with('f'))
    return parseWidth("f", ValueKind::Float);
  throw std::invalid_argument("unknown value type: " + std::string(value));
}

} // namespace cgra::ir
