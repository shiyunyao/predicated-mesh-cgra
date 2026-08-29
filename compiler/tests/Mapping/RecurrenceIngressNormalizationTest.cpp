// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGVerifier.h"
#include "cgra/Transforms/RecurrenceIngressNormalization.h"
#include "cgra/Transforms/RecurrenceIngressVerifier.h"

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
    const auto consumer2 = builder.addNode(Opcode::Sub, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto predicate = builder.addNode(Opcode::ICmp, {ValueType::i32(), ValueType::i32()}, ValueType::predicate(), ICmpPredicate::ULT);
    const auto predConsumer = builder.addNode(Opcode::Select, {ValueType::predicate(), ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()}, ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false, 4});
    builder.bindConstant(producer, 0, seed);
    builder.bindConstant(producer, 1, zero);
    builder.bindConstant(predicate, 0, seed);
    builder.bindConstant(predicate, 1, zero);
    builder.bindConstant(consumer, 1, zero);
    builder.bindConstant(consumer2, 1, zero);
    builder.bindConstant(predConsumer, 1, seed);
    builder.bindConstant(predConsumer, 2, zero);
    builder.bindConstant(store, 0, zero);
    builder.bindConstant(store, 1, seed);
    const RecurrenceBoundary boundary{{{0, ConstantRef{seed}}}};
    const RecurrenceBoundary predicateBoundary{{{0, ConstantRef{trueValue}}}};
    const auto dataEdge = builder.addDataEdge(producer, consumer, 0, 1, boundary);
    builder.addDataEdge(producer, consumer2, 0, 1, boundary);
    const auto predicateEdge = builder.addPredicateEdge(predicate, predConsumer, 0, 1, predicateBoundary);
    builder.addMemoryEdge(store, store, MemoryDepKind::WAW, 1);
    const auto original = builder.finish();
    const auto normalized = cgra::transforms::normalizeRecurrenceIngress(original);
    if (normalized.records.size() != 3)
      throw std::runtime_error("expected shared data and predicate ingress records");
    const auto report = DFGVerifier::verify(normalized.dfg);
    if (!report.ok())
      throw std::runtime_error(report.format());
    const auto ingressReport = cgra::transforms::verifyRecurrenceIngress(original, normalized);
    if (!ingressReport.ok)
      throw std::runtime_error(ingressReport.format());
    for (const auto& record : normalized.records) {
      if (!normalized.dfg.containsNode(record.ingressNode) ||
          normalized.dfg.edge(record.recurrenceEdge).distance != record.distance ||
          normalized.dfg.edge(record.localEdge).distance != 0)
        throw std::runtime_error("invalid ingress edge shape");
    }
    if (!normalized.dfg.containsEdge(dataEdge) || !normalized.dfg.containsEdge(predicateEdge))
      throw std::runtime_error("source recurrence edge IDs should remain represented");
    if (normalized.records[0].ingressNode != normalized.records[1].ingressNode ||
        !normalized.records[0].sharedIngress || normalized.records[0].consumerCount != 2)
      throw std::runtime_error("equivalent recurrence consumers should share one ingress");

    DFGBuilder longBuilder("distance_two_recurrence");
    const auto longSeed0 = longBuilder.addConstant(ValueType::i32(), 1);
    const auto longSeed1 = longBuilder.addConstant(ValueType::i32(), 2);
    const auto longProducer = longBuilder.addNode(
        Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    const auto longConsumer = longBuilder.addNode(
        Opcode::Sub, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    longBuilder.bindConstant(longProducer, 0, longSeed0);
    longBuilder.bindConstant(longProducer, 1, longSeed1);
    longBuilder.bindConstant(longConsumer, 1, longSeed1);
    const RecurrenceBoundary longBoundary{{{0, ConstantRef{longSeed0}},
                                           {1, ConstantRef{longSeed1}}}};
    const auto longEdge = longBuilder.addDataEdge(longProducer, longConsumer, 0, 2, longBoundary);
    const auto longOriginal = longBuilder.finish();
    const auto longNormalized = cgra::transforms::normalizeRecurrenceIngress(longOriginal);
    if (!longNormalized.records.empty() || !longNormalized.dfg.containsEdge(longEdge) ||
        longNormalized.diagnostics.empty())
      throw std::runtime_error("distance-two recurrence should remain unchanged by default");

    std::cout << "recurrence ingress normalization: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "recurrence ingress normalization: FAIL: " << error.what() << '\n';
    return 1;
  }
}
