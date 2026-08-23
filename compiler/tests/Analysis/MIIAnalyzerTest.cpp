// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
using cgra::analysis::MIIAnalyzer;
using cgra::analysis::MIIStatus;
using cgra::ir::DFG;
using cgra::ir::DFGBuilder;
using cgra::ir::MemoryDepKind;
using cgra::ir::MemoryOpInfo;
using cgra::ir::Opcode;
using cgra::ir::ValueType;
using cgra::target::TargetDFG;

const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

Json loadTargetJson() {
  std::ifstream input(Root / "target/cgra_v2.json");
  if (!input)
    throw std::runtime_error("cannot open canonical target contract");
  Json target;
  input >> target;
  return target;
}

class TemporaryTarget {
public:
  explicit TemporaryTarget(const Json& target) {
    static unsigned serial = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("cgra-mii-analyzer-test-" + std::to_string(serial++) + ".json");
    std::ofstream output(path_);
    output << target.dump(2) << '\n';
  }
  ~TemporaryTarget() { std::filesystem::remove(path_); }
  const auto& path() const { return path_; }

private:
  std::filesystem::path path_;
};

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json");
}

TargetDFG legalize(const DFG& generic, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

DFG makeFUPressure(unsigned count) {
  DFGBuilder builder("fu_pressure");
  const auto lhs = builder.addExternal("lhs", ValueType::i32());
  const auto rhs = builder.addExternal("rhs", ValueType::i32());
  for (unsigned index = 0; index < count; ++index) {
    const auto node =
        builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(node, 0, lhs);
    builder.bindExternal(node, 1, rhs);
  }
  return builder.finish();
}

DFG makeMulPressure(unsigned count) {
  DFGBuilder builder("mul_pressure");
  const auto lhs = builder.addExternal("lhs", ValueType::i32());
  const auto rhs = builder.addExternal("rhs", ValueType::i32());
  for (unsigned index = 0; index < count; ++index) {
    const auto node =
        builder.addNode(Opcode::Mul, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
    builder.bindExternal(node, 0, lhs);
    builder.bindExternal(node, 1, rhs);
  }
  return builder.finish();
}

DFG makeLSUPressure(unsigned count) {
  DFGBuilder builder("lsu_pressure");
  const auto address = builder.addExternal("address", ValueType::i32());
  for (unsigned index = 0; index < count; ++index) {
    const auto node = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                      std::nullopt, MemoryOpInfo{32, false});
    builder.bindExternal(node, 0, address);
  }
  return builder.finish();
}

DFG makeAddRecurrence(unsigned distance) {
  DFGBuilder builder("add_recurrence");
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.addDataEdge(add, add, 0, distance);
  builder.bindExternal(add, 1, value);
  return builder.finish();
}

DFG makeLoadRecurrence(unsigned distance) {
  DFGBuilder builder("load_recurrence");
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  builder.addDataEdge(load, load, 0, distance);
  return builder.finish();
}

DFG makeTwoNodeRecurrence(unsigned returnDistance) {
  DFGBuilder builder("two_node_recurrence");
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto first =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto second =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  builder.bindExternal(first, 1, value);
  builder.bindExternal(second, 1, value);
  builder.addDataEdge(first, second, 0, 0);
  builder.addDataEdge(second, first, 0, returnDistance);
  return builder.finish();
}

DFG makeZeroDistanceCycle() { return makeTwoNodeRecurrence(0); }

DFG makeMemoryRecurrence() {
  DFGBuilder builder("memory_recurrence");
  const auto address = builder.addExternal("address", ValueType::i32());
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto store = builder.addNode(Opcode::Store, {ValueType::i32(), ValueType::i32()},
                                     ValueType::voidTy(), std::nullopt, MemoryOpInfo{32, false});
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(store, 0, address);
  builder.bindExternal(store, 1, value);
  builder.bindExternal(load, 0, address);
  builder.addMemoryEdge(store, load, MemoryDepKind::RAW, 0);
  builder.addMemoryEdge(load, store, MemoryDepKind::WAR, 1);
  return builder.finish();
}

DFG makeCompetingRecurrences() {
  DFGBuilder builder("competing_recurrences");
  const auto value = builder.addExternal("value", ValueType::i32());
  const auto add =
      builder.addNode(Opcode::Add, {ValueType::i32(), ValueType::i32()}, ValueType::i32());
  const auto load = builder.addNode(Opcode::Load, {ValueType::i32()}, ValueType::i32(),
                                    std::nullopt, MemoryOpInfo{32, false});
  builder.bindExternal(add, 1, value);
  builder.addDataEdge(add, add, 0, 1);
  builder.addDataEdge(load, load, 0, 1);
  return builder.finish();
}

void testResourceBounds(const cgra::TargetModel& target) {
  const auto sixteen = MIIAnalyzer::analyze(legalize(makeFUPressure(16), target), target);
  expect(sixteen.ok() && sixteen.resourceMII == 1 && sixteen.resourceBreakdown.fuMII == 1,
         "sixteen FU operations fit in one II");

  const auto seventeen = MIIAnalyzer::analyze(legalize(makeFUPressure(17), target), target);
  expect(seventeen.ok() && seventeen.resourceMII == 2 && seventeen.resourceBreakdown.fuMII == 2,
         "seventeen FU operations require two II");

  const auto thirtyThree = MIIAnalyzer::analyze(legalize(makeFUPressure(33), target), target);
  expect(thirtyThree.ok() && thirtyThree.resourceMII == 3,
         "thirty-three FU operations require three II");

  const auto thirtyTwo = MIIAnalyzer::analyze(legalize(makeFUPressure(32), target), target);
  expect(thirtyTwo.ok() && thirtyTwo.resourceBreakdown.fuMII == 2,
         "thirty-two FU operations require two II");

  const auto fiveLoads = MIIAnalyzer::analyze(legalize(makeLSUPressure(5), target), target);
  expect(fiveLoads.ok() && fiveLoads.resourceBreakdown.lsuMII == 2,
         "five LSU operations require two II");

  const auto nineLoads = MIIAnalyzer::analyze(legalize(makeLSUPressure(9), target), target);
  expect(nineLoads.ok() && nineLoads.resourceBreakdown.lsuMII == 3,
         "nine LSU operations require three II");

  auto occupancyJson = loadTargetJson();
  occupancyJson["operations"]["ADD"]["issue_occupancy"] = 3;
  TemporaryTarget occupancyFile(occupancyJson);
  const auto occupancyTarget = cgra::TargetModel::loadFromFile(occupancyFile.path());
  const auto occupancyResult =
      MIIAnalyzer::analyze(legalize(makeAddRecurrence(1), occupancyTarget), occupancyTarget);
  expect(occupancyResult.ok() && occupancyResult.resourceBreakdown.selfOccupancyMII == 3 &&
             occupancyResult.resourceMII >= 3,
         "operation occupancy contributes self-occupancy MII");
}

void testRecurrenceBounds(const cgra::TargetModel& target) {
  const auto add = MIIAnalyzer::analyze(legalize(makeAddRecurrence(1), target), target);
  expect(add.ok() && add.recurrenceMII == 1, "one-cycle Add recurrence has RecMII one");

  const auto load = MIIAnalyzer::analyze(legalize(makeLoadRecurrence(1), target), target);
  if (!load.ok() || load.recurrenceMII != 2)
    throw std::runtime_error("two-cycle Load recurrence has RecMII two: " + load.format());

  const auto twoNode = MIIAnalyzer::analyze(legalize(makeTwoNodeRecurrence(1), target), target);
  expect(twoNode.ok() && twoNode.recurrenceMII == 2,
         "two-node recurrence uses the sum of intrinsic separations");

  const auto distanceTwo = MIIAnalyzer::analyze(legalize(makeTwoNodeRecurrence(2), target), target);
  expect(distanceTwo.ok() && distanceTwo.recurrenceMII == 1,
         "distance two recurrence rounds down to RecMII one");

  const auto acyclic =
      MIIAnalyzer::analyze(legalize(cgra::ir::fixtures::arithmeticChain(), target), target);
  expect(acyclic.ok() && acyclic.recurrenceMII == 1,
         "acyclic dependency chain does not inflate RecMII");

  const auto zero = MIIAnalyzer::analyze(legalize(makeZeroDistanceCycle(), target), target);
  expect(zero.status == MIIStatus::UnschedulableZeroDistanceCycle && zero.recurrenceWitness &&
             !zero.recurrenceWitness->edges.empty(),
         "zero-distance positive cycle is explicitly unschedulable");

  const auto memory = MIIAnalyzer::analyze(legalize(makeMemoryRecurrence(), target), target);
  expect(memory.ok() && memory.recurrenceMII == 2,
         "memory dependence separation contributes to RecMII");

  const auto competing = MIIAnalyzer::analyze(legalize(makeCompetingRecurrences(), target), target);
  expect(competing.ok() && competing.recurrenceMII == 2 && competing.recurrenceWitness,
         "strongest competing recurrence is selected");

  cgra::target::TargetDFG empty("empty", std::string(target.name()));
  const auto emptyResult = MIIAnalyzer::analyze(empty, target);
  expect(emptyResult.ok() && emptyResult.mii == 1, "empty valid TargetDFG has MII one");
}

void testTargetMutations(const cgra::TargetModel& canonical) {
  auto latencyJson = loadTargetJson();
  latencyJson["operations"]["ADD"]["result_latency"] = 3;
  TemporaryTarget latencyFile(latencyJson);
  const auto latencyTarget = cgra::TargetModel::loadFromFile(latencyFile.path());
  const auto latencyResult =
      MIIAnalyzer::analyze(legalize(makeAddRecurrence(1), latencyTarget), latencyTarget);
  expect(latencyResult.ok() && latencyResult.recurrenceMII == 3,
         "ADD result latency mutation changes RecMII");

  auto memoryJson = loadTargetJson();
  memoryJson["memory"]["dependence_separation"]["RAW"] = 2;
  TemporaryTarget memoryFile(memoryJson);
  const auto memoryTarget = cgra::TargetModel::loadFromFile(memoryFile.path());
  const auto memoryResult =
      MIIAnalyzer::analyze(legalize(makeMemoryRecurrence(), memoryTarget), memoryTarget);
  expect(memoryResult.ok() && memoryResult.recurrenceMII == 3,
         "memory RAW separation mutation changes RecMII");

  auto outputReadyJson = loadTargetJson();
  outputReadyJson["operations"]["ADD"]["producer_output_ready_offset"] = 2;
  TemporaryTarget outputReadyFile(outputReadyJson);
  const auto outputReadyTarget = cgra::TargetModel::loadFromFile(outputReadyFile.path());
  const auto outputReadyResult =
      MIIAnalyzer::analyze(legalize(makeAddRecurrence(1), outputReadyTarget), outputReadyTarget);
  expect(outputReadyResult.ok() && outputReadyResult.recurrenceMII == 1,
         "producer output readiness does not alter intrinsic RecMII");

  const auto result =
      MIIAnalyzer::analyze(legalize(makeTwoNodeRecurrence(1), canonical), canonical);
  const auto second =
      MIIAnalyzer::analyze(legalize(makeTwoNodeRecurrence(1), canonical), canonical);
  expect(result.toJson() == second.toJson(), "MII JSON output is deterministic");

  auto noLsuJson = loadTargetJson();
  noLsuJson["lsu"]["enabled_tiles"] = Json::array();
  TemporaryTarget noLsuFile(noLsuJson);
  const auto noLsuTarget = cgra::TargetModel::loadFromFile(noLsuFile.path());
  const auto noLsuResult =
      MIIAnalyzer::analyze(legalize(makeLSUPressure(1), canonical), noLsuTarget);
  expect(noLsuResult.status == MIIStatus::NoCompatibleResource,
         "zero LSU capacity is an explicit MII failure");

  auto capabilityJson = loadTargetJson();
  auto& defaultOperations = capabilityJson["tile_capabilities"]["default_fu_operations"];
  defaultOperations.erase(
      std::remove(defaultOperations.begin(), defaultOperations.end(), Json("MUL")),
      defaultOperations.end());
  capabilityJson["tile_capabilities"]["overrides"] =
      Json::array({{{"row", 0}, {"col", 0}, {"operations", Json::array({"MUL"})}},
                   {{"row", 1}, {"col", 0}, {"operations", Json::array({"MUL"})}}});
  TemporaryTarget capabilityFile(capabilityJson);
  const auto capabilityTarget = cgra::TargetModel::loadFromFile(capabilityFile.path());
  expect(capabilityTarget.compatibleTiles("MUL").size() == 2,
         "per-operation tile capability is target-described");
  const auto mulResult =
      MIIAnalyzer::analyze(legalize(makeMulPressure(5), capabilityTarget), capabilityTarget);
  expect(mulResult.ok() && mulResult.resourceBreakdown.perOperationMII == 3,
         "per-operation compatible capacity contributes to resource MII");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testResourceBounds(target);
    testRecurrenceBounds(target);
    testTargetMutations(target);
    std::cout << "CGRA_MII_ANALYZER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_MII_ANALYZER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
