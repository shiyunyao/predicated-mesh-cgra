// SPDX-License-Identifier: MIT
#include "Fixtures.h"

#include "cgra/IR/DFGBuilder.h"

namespace cgra::ir::fixtures {

DFG simpleAdd() {
  DFGBuilder builder("simple_add");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(add, 0, a);
  builder.bindExternal(add, 1, b);
  builder.addLiveOut("result", ValueType::i32(), add);
  return builder.finish();
}

DFG arithmeticChain() {
  DFGBuilder builder("arithmetic_chain");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto factor = builder.addConstant(ValueType::i32(), 3);
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto mul =
      builder.addNode(Opcode::Mul, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto xorr =
      builder.addNode(Opcode::Xor, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(add, 0, a);
  builder.bindExternal(add, 1, b);
  builder.addDataEdge(add, mul, 0);
  builder.bindConstant(mul, 1, factor);
  builder.addDataEdge(mul, xorr, 0);
  builder.bindExternal(xorr, 1, a);
  return builder.finish();
}

DFG fanout() {
  DFGBuilder builder("fanout");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto c = builder.addExternal("c", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto mul =
      builder.addNode(Opcode::Mul, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto sub =
      builder.addNode(Opcode::Sub, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(add, 0, a);
  builder.bindExternal(add, 1, b);
  builder.addDataEdge(add, mul, 0);
  builder.bindExternal(mul, 1, c);
  builder.addDataEdge(add, sub, 0);
  builder.bindExternal(sub, 1, c);
  return builder.finish();
}

DFG recurrence() {
  DFGBuilder builder("recurrence");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto initial = builder.addExternal("initial", ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(load, 0, address);
  builder.addDataEdge(load, add, 0);
  builder.addDataEdge(add, add, 1, 1, RecurrenceBoundary{{{0, ExternalValueRef{initial}}}});
  return builder.finish();
}

DFG loadAddStore() {
  DFGBuilder builder("load_add_store");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()},
                                     ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(load, 0, address);
  builder.addDataEdge(load, add, 0);
  builder.bindExternal(add, 1, value);
  builder.bindExternal(store, 0, address);
  builder.addDataEdge(add, store, 1);
  return builder.finish();
}

DFG predicateSelect() {
  DFGBuilder builder("predicate_select");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto yes = builder.addExternal("yes", ValueType::i32());
  const auto no = builder.addExternal("no", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::SLT);
  const auto select =
      builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()},
                      ValueType::i32());
  builder.bindExternal(cmp, 0, a);
  builder.bindExternal(cmp, 1, b);
  builder.addPredicateEdge(cmp, select, 0);
  builder.bindExternal(select, 1, yes);
  builder.bindExternal(select, 2, no);
  return builder.finish();
}

DFG predicateSelectUnsigned() {
  DFGBuilder builder("predicate_select_unsigned");
  const auto a = builder.addExternal("a", ValueType::i32());
  const auto b = builder.addExternal("b", ValueType::i32());
  const auto yes = builder.addExternal("yes", ValueType::i32());
  const auto no = builder.addExternal("no", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::ULT);
  const auto select =
      builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()},
                      ValueType::i32());
  builder.bindExternal(cmp, 0, a);
  builder.bindExternal(cmp, 1, b);
  builder.addPredicateEdge(cmp, select, 0);
  builder.bindExternal(select, 1, yes);
  builder.bindExternal(select, 2, no);
  return builder.finish();
}

DFG predicatedStore() {
  DFGBuilder builder("predicated_store");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto lhs = builder.addExternal("lhs", ValueType::i32());
  const auto rhs = builder.addExternal("rhs", ValueType::i32());
  const auto cmp = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()},
                                   ValueType::predicate(), ICmpPredicate::ULT);
  const auto store =
      builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32(), ValueType::predicate()},
                      ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(cmp, 0, lhs);
  builder.bindExternal(cmp, 1, rhs);
  builder.bindExternal(store, 0, address);
  builder.bindExternal(store, 1, value);
  builder.addPredicateEdge(cmp, store, 2);
  return builder.finish();
}

DFG memoryDependence() {
  DFGBuilder builder("memory_dependence");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()},
                                     ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(store, 0, address);
  builder.bindExternal(store, 1, value);
  builder.bindExternal(load, 0, address);
  builder.addMemoryEdge(store, load, MemoryDepKind::RAW, 1);
  return builder.finish();
}

std::vector<DFG> all() {
  return {simpleAdd(),    arithmeticChain(), fanout(),          recurrence(),
          loadAddStore(), predicateSelect(), predicatedStore(), memoryDependence()};
}

} // namespace cgra::ir::fixtures
