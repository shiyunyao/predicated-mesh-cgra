// SPDX-License-Identifier: MIT
#include "cgra/IR/Opcode.h"

#include <stdexcept>

namespace cgra::ir {

std::string_view toString(Opcode opcode) {
  switch (opcode) {
  case Opcode::Add:
    return "Add";
  case Opcode::Sub:
    return "Sub";
  case Opcode::Mul:
    return "Mul";
  case Opcode::And:
    return "And";
  case Opcode::Or:
    return "Or";
  case Opcode::Xor:
    return "Xor";
  case Opcode::Shl:
    return "Shl";
  case Opcode::LShr:
    return "LShr";
  case Opcode::AShr:
    return "AShr";
  case Opcode::ICmp:
    return "ICmp";
  case Opcode::Select:
    return "Select";
  case Opcode::Load:
    return "Load";
  case Opcode::Store:
    return "Store";
  }
  throw std::logic_error("unknown opcode");
}

Opcode opcodeFromString(std::string_view value) {
  for (const auto opcode :
       {Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::And, Opcode::Or, Opcode::Xor, Opcode::Shl,
        Opcode::LShr, Opcode::AShr, Opcode::ICmp, Opcode::Select, Opcode::Load, Opcode::Store}) {
    if (toString(opcode) == value)
      return opcode;
  }
  throw std::invalid_argument("unknown opcode: " + std::string(value));
}

std::string_view toString(ICmpPredicate predicate) {
  switch (predicate) {
  case ICmpPredicate::EQ:
    return "EQ";
  case ICmpPredicate::NE:
    return "NE";
  case ICmpPredicate::ULT:
    return "ULT";
  case ICmpPredicate::ULE:
    return "ULE";
  case ICmpPredicate::UGT:
    return "UGT";
  case ICmpPredicate::UGE:
    return "UGE";
  case ICmpPredicate::SLT:
    return "SLT";
  case ICmpPredicate::SLE:
    return "SLE";
  case ICmpPredicate::SGT:
    return "SGT";
  case ICmpPredicate::SGE:
    return "SGE";
  }
  throw std::logic_error("unknown compare predicate");
}

ICmpPredicate icmpPredicateFromString(std::string_view value) {
  for (const auto predicate :
       {ICmpPredicate::EQ, ICmpPredicate::NE, ICmpPredicate::ULT, ICmpPredicate::ULE,
        ICmpPredicate::UGT, ICmpPredicate::UGE, ICmpPredicate::SLT, ICmpPredicate::SLE,
        ICmpPredicate::SGT, ICmpPredicate::SGE}) {
    if (toString(predicate) == value)
      return predicate;
  }
  throw std::invalid_argument("unknown compare predicate: " + std::string(value));
}

} // namespace cgra::ir
