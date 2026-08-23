// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cgra::ir {

enum class ValueKind {
  Integer,
  Predicate,
  Float,
  Void,
};

struct ValueType {
  ValueKind kind = ValueKind::Void;
  std::uint16_t bitWidth = 0;

  static constexpr ValueType integer(std::uint16_t bits) { return {ValueKind::Integer, bits}; }
  static constexpr ValueType predicate() { return {ValueKind::Predicate, 1}; }
  static constexpr ValueType floating(std::uint16_t bits) { return {ValueKind::Float, bits}; }
  static constexpr ValueType i1() { return predicate(); }
  static constexpr ValueType i8() { return integer(8); }
  static constexpr ValueType i16() { return integer(16); }
  static constexpr ValueType i32() { return integer(32); }
  static constexpr ValueType f32() { return floating(32); }
  static constexpr ValueType voidTy() { return {ValueKind::Void, 0}; }

  std::string toString() const;
  static ValueType fromString(std::string_view value);

  friend constexpr bool operator==(const ValueType&, const ValueType&) = default;
};

std::string_view toString(ValueKind kind);
ValueKind valueKindFromString(std::string_view value);

} // namespace cgra::ir
