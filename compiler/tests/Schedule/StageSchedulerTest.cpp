// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Schedule/StageAssignmentVerifier.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Schedule/StagedMappingSerialization.h"
#include "cgra/Target/TargetLegalizer.h"

#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace cgra::schedule {

class StagedMappingTestAccess {
public:
  static StagedMapping make(cgra::mapping::ModuloMapping modulo, std::vector<NodeStage> stages) {
    return StagedMapping(std::move(modulo), std::move(stages));
  }
};

} // namespace cgra::schedule

namespace {

using namespace cgra::schedule;
using cgra::mapping::Direction;
using cgra::mapping::LinkStep;
using cgra::mapping::ModuloMapping;
using cgra::mapping::ModuloMappingBuilder;
using cgra::mapping::ModuloSlot;
using cgra::mapping::NetworkDomain;
using cgra::mapping::TransportPlan;
using cgra::mapping::VirtualHold;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json");
}

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

ModuloMapping arithmeticMapping(const cgra::target::TargetDFG& dfg, std::uint32_t ii,
                                std::uint32_t sourceSlot = 0, std::uint32_t destinationSlot = 0) {
  ModuloMappingBuilder builder(dfg, ii);
  for (const auto& node : dfg.nodes())
    builder.place(node.id, {0, node.id}, ModuloSlot(node.id == 0 ? sourceSlot : destinationSlot));
  for (const auto& edge : dfg.edges()) {
    const auto source = dfg.node(edge.src);
    const auto sourceTile = cgra::mapping::TileCoord{0, edge.src};
    const auto ready = source.producerOutputReadyOffset.value_or(0U);
    builder.setTransport(
        edge.id, TransportPlan{edge.id,
                               NetworkDomain::Data,
                               {LinkStep{NetworkDomain::Data, sourceTile, Direction::East, ready}},
                               ready + 1});
  }
  return builder.finish();
}

ModuloMapping recurrenceMapping(const cgra::target::TargetDFG& dfg, std::uint32_t ii,
                                std::uint32_t selfRelease) {
  ModuloMappingBuilder builder(dfg, ii);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.setTransport(0, TransportPlan{0,
                                        NetworkDomain::Data,
                                        {LinkStep{NetworkDomain::Data, {0, 0}, Direction::East, 2}},
                                        3});
  builder.setTransport(1, TransportPlan{1,
                                        NetworkDomain::Data,
                                        {VirtualHold{NetworkDomain::Data, {0, 1}, 0, selfRelease}},
                                        selfRelease});
  return builder.finish();
}

ModuloMapping memoryMapping(const cgra::target::TargetDFG& dfg, std::uint32_t ii) {
  ModuloMappingBuilder builder(dfg, ii);
  std::uint32_t slot = 0;
  for (const auto& node : dfg.nodes())
    builder.place(node.id, {0, 0}, ModuloSlot(slot++));
  for (const auto& edge : dfg.edges())
    builder.setMemorySeparation(edge.id, 1);
  return builder.finish();
}

void testSignedCeilDivision() {
  const std::pair<std::int64_t, std::int64_t> cases[] = {{9, 3},   {8, 2},   {7, 2},   {1, 1},
                                                         {0, 0},   {-1, 0},  {-2, 0},  {-3, 0},
                                                         {-4, -1}, {-5, -1}, {-8, -2}, {-9, -2}};
  for (const auto& [numerator, expected] : cases)
    expect(ceilDivSigned(numerator, 4) == expected, "signed ceil division corpus mismatch");
  expect(ceilDivSigned(5, 4) == 2, "positive signed ceil division");
  expect(ceilDivSigned(-3, 4) == 0, "negative signed ceil division");
  bool rejected = false;
  try {
    (void)ceilDivSigned(1, 0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "signed ceil division rejects zero denominator");
}

void testForwardAndWrap(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  const auto mapping = arithmeticMapping(dfg, 4, 3, 0);
  expect(cgra::mapping::ModuloMappingVerifier::verify(dfg, target, mapping).ok(),
         "wrap mapping passes T005");
  const auto result = StageScheduler::schedule(dfg, target, mapping);
  expect(result.ok(), "forward modulo mapping schedules");
  expect(result.mapping->stage(0) == 0 && result.mapping->stage(1) == 1,
         "slot wrap requires one stage");
  expect(result.mapping->logicalIssueTime(1) == 4, "logical issue time derives slot and stage");
  expect(StageAssignmentVerifier::verify(dfg, target, *result.mapping).ok(),
         "scheduled mapping independently verifies");

  const auto negativeMapping = arithmeticMapping(dfg, 4, 0, 3);
  const auto negative = StageScheduler::schedule(dfg, target, negativeMapping);
  expect(negative.ok() && negative.mapping->stage(1) == 0,
         "negative constraint numerator does not force a stage");
}

void testRecurrenceBoundary(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  const auto feasible = recurrenceMapping(dfg, 4, 4);
  expect(cgra::mapping::ModuloMappingVerifier::verify(dfg, target, feasible).ok(),
         "feasible recurrence mapping passes T005");
  const auto scheduled = StageScheduler::schedule(dfg, target, feasible);
  expect(scheduled.ok(), "T009 schedules a concrete feasible recurrence");

  const auto infeasible = recurrenceMapping(dfg, 4, 5);
  expect(cgra::mapping::ModuloMappingVerifier::verify(dfg, target, infeasible).ok(),
         "concrete recurrence boundary remains T005-valid");
  const auto rejected = StageScheduler::schedule(dfg, target, infeasible);
  expect(rejected.status == StageSchedulingStatus::InfeasibleStageConstraints &&
             !rejected.mapping && rejected.witness && rejected.witness->totalMinimumStageDelta > 0,
         "T009 rejects a positive concrete recurrence cycle without remapping");
}

void testMixedSignedCycle(const cgra::TargetModel& target) {
  cgra::ir::DFGBuilder generic("mixed_signed_cycle");
  const auto left = generic.addExternal("left", cgra::ir::ValueType::i32());
  const auto right = generic.addExternal("right", cgra::ir::ValueType::i32());
  const auto first = generic.addNode(cgra::ir::Opcode::Add,
                                     {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                     cgra::ir::ValueType::i32());
  const auto second = generic.addNode(cgra::ir::Opcode::Add,
                                      {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                      cgra::ir::ValueType::i32());
  generic.bindExternal(first, 0, left);
  generic.bindExternal(second, 0, right);
  generic.addDataEdge(first, second, 1);
  generic.addDataEdge(second, first, 1);
  const auto dfg = legalize(generic.finish(), target);
  ModuloMappingBuilder builder(dfg, 4);
  builder.place(0, {0, 0}, ModuloSlot(3));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.setTransport(0, TransportPlan{0,
                                        NetworkDomain::Data,
                                        {LinkStep{NetworkDomain::Data, {0, 0}, Direction::East, 0}},
                                        1});
  builder.setTransport(1, TransportPlan{1,
                                        NetworkDomain::Data,
                                        {LinkStep{NetworkDomain::Data, {0, 1}, Direction::West, 0}},
                                        1});
  const auto mapping = builder.finish();
  expect(cgra::mapping::ModuloMappingVerifier::verify(dfg, target, mapping).ok(),
         "mixed signed cycle mapping passes T005");
  const auto result = StageScheduler::schedule(dfg, target, mapping);
  expect(result.status == StageSchedulingStatus::InfeasibleStageConstraints,
         "positive cycle with a negative edge delta is detected");
}

void testMemoryAndSerialization(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  const auto modulo = memoryMapping(dfg, 4);
  const auto result = StageScheduler::schedule(dfg, target, modulo);
  if (!result.ok())
    throw std::runtime_error("memory ordering edge uses the uniform stage equation: " +
                             result.format());
  const auto text = StagedMappingSerialization::toJson(*result.mapping);
  const auto roundTrip = StagedMappingSerialization::parse(text);
  expect(roundTrip == *result.mapping, "staged mapping JSON round-trips");
  expect(text == StagedMappingSerialization::toJson(roundTrip),
         "staged mapping serialization is deterministic");
}

void testIndependentCorruption(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  ModuloMappingBuilder builder(dfg, 4);
  builder.place(0, {0, 0}, ModuloSlot(0));
  const auto modulo = builder.finish();
  const auto missing = StagedMappingTestAccess::make(modulo, {});
  expect(!StageAssignmentVerifier::verify(dfg, target, missing).ok(),
         "independent verifier rejects missing stage");
  const auto tooLarge =
      StagedMappingTestAccess::make(modulo, {{0, std::numeric_limits<PipelineStage>::max()}});
  expect(StageAssignmentVerifier::verify(dfg, target, tooLarge)
             .contains(StageAssignmentDiagnosticCode::STAGE_OUTPUT_ARITHMETIC_OVERFLOW),
         "independent verifier detects logical-time overflow");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testSignedCeilDivision();
    testForwardAndWrap(target);
    testRecurrenceBoundary(target);
    testMixedSignedCycle(target);
    testMemoryAndSerialization(target);
    testIndependentCorruption(target);
    std::cout << "stage scheduler tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "stage scheduler tests failed: " << error.what() << '\n';
    return 1;
  }
}
