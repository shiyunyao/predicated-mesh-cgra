// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace cgra::ir {

enum class Opcode {
  Add,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  ICmp,
  Select,
  Load,
  Store,
};

enum class ICmpPredicate {
  EQ,
  NE,
  ULT,
  ULE,
  UGT,
  UGE,
  SLT,
  SLE,
  SGT,
  SGE,
};

std::string_view toString(Opcode opcode);
Opcode opcodeFromString(std::string_view value);
std::string_view toString(ICmpPredicate predicate);
ICmpPredicate icmpPredicateFromString(std::string_view value);

} // namespace cgra::ir
