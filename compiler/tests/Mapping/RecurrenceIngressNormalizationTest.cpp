// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGVerifier.h"
#include "cgra/Transforms/RecurrenceIngressNormalization.h"

#include <iostream>
#include <stdexcept>

int main() {
  try {
    using namespace cgra::ir;
    DFGBuilder builder("recurrence_ingress_test");
    const auto seed = builder.addConstant(ValueType::i32(), 7);
    const auto zero = builder.addConstant(ValueType::i32(), 0);
    const auto trueValue = builder.addConstant(ValueType::predicate(), 1);
    const auto producer = builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto consumer = builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto predicate = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()}, ValueType::predicate(), ICmpPredicate::ULT);
    const auto predConsumer = builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()}, ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false, 4});
    builder.bindConstant(producer, 0, seed);
    builder.bindConstant(producer, 1, zero);
    builder.bindConstant(predicate, 0, seed);
    builder.bindConstant(predicate, 1, zero);
    builder.bindConstant(consumer, 1, zero);
    builder.bindConstant(predConsumer, 1, seed);
    builder.bindConstant(predConsumer, 2, zero);
    builder.bindConstant(store, 0, zero);
    builder.bindConstant(store, 1, seed);
    const RecurrenceBoundary boundary{{{0, ConstantRef{seed}}}};
    const RecurrenceBoundary predicateBoundary{{{0, ConstantRef{trueValue}}}};
    const auto dataEdge = builder.addDataEdge(producer, consumer, 0, 1, boundary);
    const auto predicateEdge = builder.addPredicateEdge(predicate, predConsumer, 0, 1, predicateBoundary);
    builder.addMemoryEdge(store, store, MemoryDepKind::WAW, 1);
    const auto original = builder.finish();
    const auto normalized = cgra::transforms::normalizeRecurrenceIngress(original);
    if (normalized.records.size() != 2)
      throw std::runtime_error("expected data and predicate ingress records");
    const auto report = DFGVerifier::verify(normalized.dfg);
    if (!report.ok())
      throw std::runtime_error(report.format());
    for (const auto& record : normalized.records) {
      if (!normalized.dfg.containsNode(record.ingressNode) ||
          normalized.dfg.edge(record.recurrenceEdge).distance != record.distance ||
          normalized.dfg.edge(record.localEdge).distance != 0)
        throw std::runtime_error("invalid ingress edge shape");
    }
    if (!normalized.dfg.containsEdge(dataEdge) || !normalized.dfg.containsEdge(predicateEdge))
      throw std::runtime_error("source recurrence edge IDs should remain represented");
    std::cout << "recurrence ingress normalization: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "recurrence ingress normalization: FAIL: " << error.what() << '\n';
    return 1;
  }
}
