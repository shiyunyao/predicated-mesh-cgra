// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetDFGVerifier.h"
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
const std::filesystem::path RepositoryRoot = CGRA_REPOSITORY_ROOT;
const std::filesystem::path TargetPath = RepositoryRoot / "target/cgra_v2.json";

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

Json loadTargetJson() {
  std::ifstream stream(TargetPath);
  if (!stream)
    throw std::runtime_error("cannot open canonical target");
  Json target;
  stream >> target;
  return target;
}

class TemporaryTarget {
public:
  explicit TemporaryTarget(const Json& target) {
    static unsigned serial = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("cgra-target-legalizer-test-" + std::to_string(serial++) + ".json");
    std::ofstream stream(path_);
    stream << target.dump(2) << '\n';
  }
  ~TemporaryTarget() { std::filesystem::remove(path_); }
  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

void verifyEdgePreservation(const cgra::ir::DFG& generic,
                            const cgra::target::TargetLegalizationResult& result) {
  expect(result.dfg.has_value(), "edge oracle requires a Target DFG");
  const auto& target = *result.dfg;
  expect(target.edges().size() == generic.edges().size(), "V0 preserves edge count");
  for (const auto& edge : generic.edges()) {
    const auto srcIt = result.map.genericToTarget.find(edge.src);
    const auto dstIt = result.map.genericToTarget.find(edge.dst);
    expect(srcIt != result.map.genericToTarget.end() && srcIt->second.size() == 1,
           "Generic source has one Target origin");
    expect(dstIt != result.map.genericToTarget.end() && dstIt->second.size() == 1,
           "Generic destination has one Target origin");
    const auto mappedSrc = srcIt->second.front();
    const auto mappedDst = dstIt->second.front();
    const auto matches =
        std::count_if(target.edges().begin(), target.edges().end(), [&](const auto& candidate) {
          return candidate.src == mappedSrc && candidate.dst == mappedDst &&
                 candidate.kind() == edge.kind() && candidate.distance == edge.distance &&
                 candidate.info == edge.info;
        });
    expect(matches == 1, "Generic edge maps to exactly one identical Target edge");
  }
}

void testTargetOperationQueries(const cgra::TargetModel& target) {
  expect(target.supportsValueType(cgra::ir::ValueType::i32()), "target supports i32");
  expect(target.supportsValueType(cgra::ir::ValueType::predicate()), "target supports predicate");
  expect(target.supportsValueType(cgra::ir::ValueType::voidTy()), "target supports void");
  expect(!target.supportsValueType(cgra::ir::ValueType::i16()), "target rejects i16");
  expect(target.findOperation("ADD") != nullptr, "target exposes ADD");
  expect(target.operation("ADD").issueOccupancy == 1 && target.operation("ADD").resultLatency == 1,
         "ADD timing");
  expect(target.operation("LOAD").executionClass == cgra::TargetExecutionClass::LSU &&
             target.operation("LOAD").resultLatency == 2,
         "LOAD semantics");
  expect(!target.operation("STORE").resultLatency &&
             target.operation("STORE").resultType == cgra::ir::ValueType::voidTy(),
         "STORE semantics");
}

void testPositiveFixtures(const cgra::TargetModel& target) {
  for (const auto& fixture : cgra::ir::fixtures::all()) {
    if (fixture.name() == "predicate_select")
      continue;
    const auto result = cgra::target::TargetLegalizer::legalize(fixture, target);
    if (!result.ok())
      throw std::runtime_error("canonical fixture legalization failed: " + fixture.name() + "\n" +
                               result.format());
    expect(result.dfg.has_value(), "successful legalization has Target DFG");
    expect(result.dfg->nodes().size() == fixture.nodes().size(), "one-to-one node count");
    expect(result.map.genericToTarget.size() == fixture.nodes().size(), "provenance map coverage");
    verifyEdgePreservation(fixture, result);
    const auto targetReport =
        cgra::target::TargetDFGVerifier::verify(*result.dfg, target, &fixture);
    if (!targetReport.ok())
      throw std::runtime_error("legalized fixture TargetDFGVerifier failure: " +
                               targetReport.format() + "\n" + cgra::target::dump(*result.dfg));
    expect(cgra::target::parse(cgra::target::toJson(*result.dfg)) == *result.dfg,
           "Target DFG JSON round-trip");
  }
  const auto unsignedPredicate = cgra::ir::fixtures::predicateSelectUnsigned();
  const auto result = cgra::target::TargetLegalizer::legalize(unsignedPredicate, target);
  expect(result.ok(), "target-supported predicate/select fixture legalizes");
  verifyEdgePreservation(unsignedPredicate, result);

  cgra::ir::DFGBuilder memoryEdges("memory_edge_kinds");
  const auto address = memoryEdges.addExternal("address", cgra::ir::ValueType::i32());
  const auto value = memoryEdges.addExternal("value", cgra::ir::ValueType::i32());
  const auto store0 = memoryEdges.addNode(
      cgra::ir::Opcode::Store, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::voidTy(), std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  const auto load = memoryEdges.addNode(cgra::ir::Opcode::Load, {cgra::ir::ValueType::i32()},
                                        cgra::ir::ValueType::i32(), std::nullopt,
                                        cgra::ir::MemoryOpInfo{32, false});
  const auto store1 = memoryEdges.addNode(
      cgra::ir::Opcode::Store, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::voidTy(), std::nullopt, cgra::ir::MemoryOpInfo{32, false});
  for (const auto store : {store0, store1}) {
    memoryEdges.bindExternal(store, 0, address);
    memoryEdges.bindExternal(store, 1, value);
  }
  memoryEdges.bindExternal(load, 0, address);
  memoryEdges.addMemoryEdge(store0, load, cgra::ir::MemoryDepKind::RAW, 0);
  memoryEdges.addMemoryEdge(load, store1, cgra::ir::MemoryDepKind::WAR, 0);
  memoryEdges.addMemoryEdge(store0, store1, cgra::ir::MemoryDepKind::WAW, 1);
  const auto memoryGraph = memoryEdges.finish();
  const auto memoryResult = cgra::target::TargetLegalizer::legalize(memoryGraph, target);
  expect(memoryResult.ok(), "all memory dependence kinds legalize");
  verifyEdgePreservation(memoryGraph, memoryResult);
}

void testUnsupportedGenericOperations(const cgra::TargetModel& target) {
  cgra::ir::DFGBuilder ashrBuilder("ashr_i32");
  const auto value = ashrBuilder.addExternal("value", cgra::ir::ValueType::i32());
  const auto shift = ashrBuilder.addNode(cgra::ir::Opcode::AShr,
                                         {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                         cgra::ir::ValueType::i32());
  ashrBuilder.bindExternal(shift, 0, value);
  ashrBuilder.bindExternal(shift, 1, value);
  const auto ashr = ashrBuilder.finish();
  expect(cgra::ir::DFGVerifier::verify(ashr).ok(), "AShr is Generic-valid");
  const auto ashrResult = cgra::target::TargetLegalizer::legalize(ashr, target);
  expect(!ashrResult.ok() && !ashrResult.dfg, "AShr is target-unsupported");
  expect(ashrResult.toJson().find("TLEG_MISSING_TARGET_OPERATION") != std::string::npos,
         "AShr failure code");

  cgra::ir::DFGBuilder cmpBuilder("icmp_slt_i32");
  const auto lhs = cmpBuilder.addExternal("lhs", cgra::ir::ValueType::i32());
  const auto rhs = cmpBuilder.addExternal("rhs", cgra::ir::ValueType::i32());
  const auto cmp = cmpBuilder.addNode(
      cgra::ir::Opcode::ICmp, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::predicate(), cgra::ir::ICmpPredicate::SLT);
  cmpBuilder.bindExternal(cmp, 0, lhs);
  cmpBuilder.bindExternal(cmp, 1, rhs);
  const auto signedCmp = cmpBuilder.finish();
  const auto cmpResult = cgra::target::TargetLegalizer::legalize(signedCmp, target);
  expect(!cmpResult.ok() && !cmpResult.dfg, "signed compare is target-unsupported");
  expect(cmpResult.toJson().find("TLEG_UNSUPPORTED_ICMP_PREDICATE") != std::string::npos ||
             cmpResult.toJson().find("TLEG_MISSING_TARGET_OPERATION") != std::string::npos,
         "signed compare failure code");

  cgra::ir::DFGBuilder i16Builder("i16_add");
  const auto i16Value = i16Builder.addExternal("value", cgra::ir::ValueType::i16());
  const auto i16Add = i16Builder.addNode(cgra::ir::Opcode::Add,
                                         {cgra::ir::ValueType::i16(), cgra::ir::ValueType::i16()},
                                         cgra::ir::ValueType::i16());
  i16Builder.bindExternal(i16Add, 0, i16Value);
  i16Builder.bindExternal(i16Add, 1, i16Value);
  const auto i16Graph = i16Builder.finish();
  expect(cgra::ir::DFGVerifier::verify(i16Graph).ok(), "i16 Add is Generic-valid");
  const auto i16Result = cgra::target::TargetLegalizer::legalize(i16Graph, target);
  expect(!i16Result.ok() && !i16Result.dfg &&
             i16Result.toJson().find("TLEG_UNSUPPORTED_TYPE") != std::string::npos,
         "target rejects unsupported i16 Add");

  cgra::ir::DFGBuilder f32Builder("f32_mul");
  const auto f32Value = f32Builder.addExternal("value", cgra::ir::ValueType::f32());
  const auto f32Mul = f32Builder.addNode(cgra::ir::Opcode::Mul,
                                         {cgra::ir::ValueType::f32(), cgra::ir::ValueType::f32()},
                                         cgra::ir::ValueType::f32());
  f32Builder.bindExternal(f32Mul, 0, f32Value);
  f32Builder.bindExternal(f32Mul, 1, f32Value);
  const auto f32Graph = f32Builder.finish();
  expect(cgra::ir::DFGVerifier::verify(f32Graph).ok(), "f32 Mul is Generic-valid");
  const auto f32Result = cgra::target::TargetLegalizer::legalize(f32Graph, target);
  expect(!f32Result.ok() && !f32Result.dfg &&
             f32Result.toJson().find("TLEG_UNSUPPORTED_TYPE") != std::string::npos,
         "target rejects unsupported f32 Mul");

  cgra::ir::DFGBuilder widthBuilder("load_width_16");
  const auto address = widthBuilder.addExternal("address", cgra::ir::ValueType::i32());
  const auto load = widthBuilder.addNode(cgra::ir::Opcode::Load, {cgra::ir::ValueType::i32()},
                                         cgra::ir::ValueType::i32(), std::nullopt,
                                         cgra::ir::MemoryOpInfo{16, false});
  widthBuilder.bindExternal(load, 0, address);
  const auto widthGraph = widthBuilder.finish();
  expect(cgra::ir::DFGVerifier::verify(widthGraph).ok(), "16-bit Load is Generic-valid");
  const auto widthResult = cgra::target::TargetLegalizer::legalize(widthGraph, target);
  expect(!widthResult.ok() && !widthResult.dfg &&
             widthResult.toJson().find("TLEG_UNSUPPORTED_MEMORY_ACCESS_WIDTH") != std::string::npos,
         "target rejects unsupported memory access width");
}

void testTargetMutation(const cgra::ir::DFG& fixture) {
  auto targetJson = loadTargetJson();
  targetJson["operations"]["ADD"]["result_latency"] = 3;
  targetJson["latencies"]["fu_ops"]["ADD"] = 3;
  TemporaryTarget file(targetJson);
  const auto target = cgra::TargetModel::loadFromFile(file.path());
  const auto result = cgra::target::TargetLegalizer::legalize(fixture, target);
  expect(result.ok(), "mutated target remains legal");
  const auto add = std::find_if(result.dfg->nodes().begin(), result.dfg->nodes().end(),
                                [](const auto& node) { return node.operation == "ADD"; });
  expect(add != result.dfg->nodes().end() && add->resultLatency == 3,
         "legalized latency follows TargetModel");

  targetJson = loadTargetJson();
  targetJson["operations"]["ADD"]["issue_occupancy"] = 3;
  targetJson["latencies"]["issue_occupancy"]["fu"] = 3;
  TemporaryTarget occupancyFile(targetJson);
  const auto occupancyTarget = cgra::TargetModel::loadFromFile(occupancyFile.path());
  const auto occupancyResult = cgra::target::TargetLegalizer::legalize(fixture, occupancyTarget);
  const auto occupiedAdd =
      std::find_if(occupancyResult.dfg->nodes().begin(), occupancyResult.dfg->nodes().end(),
                   [](const auto& node) { return node.operation == "ADD"; });
  expect(occupancyResult.ok() && occupiedAdd != occupancyResult.dfg->nodes().end() &&
             occupiedAdd->issueOccupancy == 3,
         "legalized occupancy follows TargetModel");

  targetJson = loadTargetJson();
  targetJson["operations"]["ADD"]["producer_output_ready_offset"] = 2;
  targetJson["latencies"]["producer_output_ready_offsets"]["fu"] = 2;
  TemporaryTarget outputFile(targetJson);
  const auto outputTarget = cgra::TargetModel::loadFromFile(outputFile.path());
  const auto outputResult = cgra::target::TargetLegalizer::legalize(fixture, outputTarget);
  const auto outputAdd =
      std::find_if(outputResult.dfg->nodes().begin(), outputResult.dfg->nodes().end(),
                   [](const auto& node) { return node.operation == "ADD"; });
  expect(outputResult.ok() && outputAdd != outputResult.dfg->nodes().end() &&
             outputAdd->producerOutputReadyOffset == 2,
         "legalized output readiness follows TargetModel");

  targetJson = loadTargetJson();
  targetJson["operations"].erase("ADD");
  TemporaryTarget missingOperation(targetJson);
  const auto noAddTarget = cgra::TargetModel::loadFromFile(missingOperation.path());
  const auto failed = cgra::target::TargetLegalizer::legalize(fixture, noAddTarget);
  expect(!failed.ok() && !failed.dfg, "removing ADD rejects Generic Add");
}

void testTargetDrivenOperations() {
  auto targetJson = loadTargetJson();
  targetJson["operations"]["ASHR"] = targetJson["operations"]["LSHR"];
  TemporaryTarget ashrFile(targetJson);
  const auto ashrTarget = cgra::TargetModel::loadFromFile(ashrFile.path());
  cgra::ir::DFGBuilder ashrBuilder("ashr_target_driven");
  const auto value = ashrBuilder.addExternal("value", cgra::ir::ValueType::i32());
  const auto shift = ashrBuilder.addNode(cgra::ir::Opcode::AShr,
                                         {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                         cgra::ir::ValueType::i32());
  ashrBuilder.bindExternal(shift, 0, value);
  ashrBuilder.bindExternal(shift, 1, value);
  const auto ashrResult = cgra::target::TargetLegalizer::legalize(ashrBuilder.finish(), ashrTarget);
  expect(ashrResult.ok(), "AShr support follows TargetModel descriptor");

  targetJson = loadTargetJson();
  targetJson["operations"]["CMP_SLT"] = targetJson["operations"]["CMP_ULT"];
  TemporaryTarget sltFile(targetJson);
  const auto sltTarget = cgra::TargetModel::loadFromFile(sltFile.path());
  cgra::ir::DFGBuilder sltBuilder("slt_target_driven");
  const auto lhs = sltBuilder.addExternal("lhs", cgra::ir::ValueType::i32());
  const auto rhs = sltBuilder.addExternal("rhs", cgra::ir::ValueType::i32());
  const auto cmp = sltBuilder.addNode(
      cgra::ir::Opcode::ICmp, {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::predicate(), cgra::ir::ICmpPredicate::SLT);
  sltBuilder.bindExternal(cmp, 0, lhs);
  sltBuilder.bindExternal(cmp, 1, rhs);
  const auto sltResult = cgra::target::TargetLegalizer::legalize(sltBuilder.finish(), sltTarget);
  expect(sltResult.ok(), "signed compare support follows TargetModel descriptor");

  targetJson = loadTargetJson();
  targetJson["operations"].erase("CMP_ULT");
  TemporaryTarget noUltFile(targetJson);
  const auto noUltTarget = cgra::TargetModel::loadFromFile(noUltFile.path());
  const auto unsignedResult = cgra::target::TargetLegalizer::legalize(
      cgra::ir::fixtures::predicateSelectUnsigned(), noUltTarget);
  expect(!unsignedResult.ok() && !unsignedResult.dfg, "removing CMP_ULT rejects Generic ULT");

  auto noLsuJson = loadTargetJson();
  noLsuJson["lsu"]["enabled_tiles"] = Json::array();
  TemporaryTarget noLsuFile(noLsuJson);
  const auto noLsuTarget = cgra::TargetModel::loadFromFile(noLsuFile.path());
  const auto loadResult =
      cgra::target::TargetLegalizer::legalize(cgra::ir::fixtures::recurrence(), noLsuTarget);
  expect(!loadResult.ok() && !loadResult.dfg &&
             loadResult.toJson().find("TLEG_NO_COMPATIBLE_EXECUTION_RESOURCE") != std::string::npos,
         "zero-LSU target rejects Load");
  const auto storeResult =
      cgra::target::TargetLegalizer::legalize(cgra::ir::fixtures::loadAddStore(), noLsuTarget);
  expect(!storeResult.ok() && !storeResult.dfg &&
             storeResult.toJson().find("TLEG_NO_COMPATIBLE_EXECUTION_RESOURCE") !=
                 std::string::npos,
         "zero-LSU target rejects Store");
}

} // namespace

int main() {
  try {
    const auto target = cgra::TargetModel::loadFromFile(TargetPath);
    testTargetOperationQueries(target);
    const auto fixture = cgra::ir::fixtures::simpleAdd();
    testPositiveFixtures(target);
    testUnsupportedGenericOperations(target);
    testTargetDrivenOperations();
    testTargetMutation(fixture);
    std::cout << "CGRA_TARGET_LEGALIZER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_TARGET_LEGALIZER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
