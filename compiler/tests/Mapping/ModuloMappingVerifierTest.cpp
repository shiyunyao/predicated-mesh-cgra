// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cgra::mapping {

class ModuloMappingTestAccess {
public:
  static void duplicatePlacement(ModuloMapping& mapping) {
    mapping.placements_.push_back(mapping.placements_.front());
  }
  static void setRequiredSeparation(ModuloMapping& mapping, std::uint32_t separation) {
    mapping.dependences_.front().requiredSeparationCycles = separation;
  }
};

} // namespace cgra::mapping

namespace {

using Json = nlohmann::json;
using namespace cgra::mapping;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json");
}

Json loadTargetJson() {
  std::ifstream input(Root / "target/cgra_v2.json");
  Json value;
  input >> value;
  return value;
}

class TemporaryTarget {
public:
  explicit TemporaryTarget(const Json& value) {
    static unsigned serial = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("cgra-modulo-mapping-verifier-" + std::to_string(serial++) + ".json");
    std::ofstream output(path_);
    output << value.dump(2) << '\n';
  }
  ~TemporaryTarget() { std::filesystem::remove(path_); }
  const auto& path() const { return path_; }

private:
  std::filesystem::path path_;
};

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

ModuloMapping makeSimpleMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  ModuloMappingBuilder builder(dfg, 1);
  builder.place(0, {0, 0}, ModuloSlot(0));
  return builder.finish();
}

ModuloMapping makeChainMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloMappingBuilder builder(dfg, 2);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.place(2, {0, 2}, ModuloSlot(0));
  for (const auto& edge : dfg.edges()) {
    const auto source = edge.src == 0 ? TileCoord{0, 0} : TileCoord{0, 1};
    builder.setTransport(edge.id, {edge.id,
                                   NetworkDomain::Data,
                                   {LinkStep{NetworkDomain::Data, source, Direction::East, 0}},
                                   1});
  }
  return builder.finish();
}

ModuloMapping makeFanoutMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::fanout(), target);
  ModuloMappingBuilder builder(dfg, 1);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.place(2, {1, 0}, ModuloSlot(0));
  for (const auto& edge : dfg.edges()) {
    const auto direction = edge.dst == 1 ? Direction::East : Direction::South;
    builder.setTransport(
        edge.id,
        {edge.id, NetworkDomain::Data, {LinkStep{NetworkDomain::Data, {0, 0}, direction, 0}}, 1});
  }
  return builder.finish();
}

ModuloMapping makePredicateMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::predicateSelectUnsigned(), target);
  ModuloMappingBuilder builder(dfg, 1);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.setTransport(0, {0,
                           NetworkDomain::Predicate,
                           {LinkStep{NetworkDomain::Predicate, {0, 0}, Direction::East, 0}},
                           1});
  return builder.finish();
}

ModuloMapping makeLoadMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  ModuloMappingBuilder builder(dfg, 2);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.setTransport(
      0, {0, NetworkDomain::Data, {LinkStep{NetworkDomain::Data, {0, 0}, Direction::East, 2}}, 3});
  builder.setTransport(
      1, {1, NetworkDomain::Data, {VirtualHold{NetworkDomain::Data, {0, 1}, 0, 1}}, 1});
  return builder.finish();
}

ModuloMapping makeMemoryMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  ModuloMappingBuilder builder(dfg, 1);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {1, 0}, ModuloSlot(0));
  builder.setMemorySeparation(0, target.memoryDependenceSeparation(cgra::ir::MemoryDepKind::RAW));
  return builder.finish();
}

ModuloMapping makeCoissueMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  ModuloMappingBuilder builder(dfg, 4);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 0}, ModuloSlot(0));
  builder.place(2, {1, 0}, ModuloSlot(0));
  builder.setTransport(
      0, {0, NetworkDomain::Data, {VirtualHold{NetworkDomain::Data, {0, 0}, 2, 3}}, 3});
  builder.setTransport(
      1, {1, NetworkDomain::Data, {LinkStep{NetworkDomain::Data, {0, 0}, Direction::South, 0}}, 1});
  return builder.finish();
}

ModuloMapping makeInvalidThenValidRouteMapping(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  ModuloMappingBuilder builder(dfg, 2);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 0}, ModuloSlot(0));
  builder.place(2, {0, 1}, ModuloSlot(0));
  builder.setTransport(0, {0,
                           NetworkDomain::Data,
                           {LinkStep{NetworkDomain::Data, {0, 0}, Direction::East, 2},
                            VirtualHold{NetworkDomain::Data, {1, 1}, 2, 3}},
                           3});
  builder.setTransport(
      1, {1, NetworkDomain::Data, {LinkStep{NetworkDomain::Data, {0, 0}, Direction::East, 0}}, 1});
  return builder.finish();
}

void testPositiveMappings(const cgra::TargetModel& target) {
  const auto simpleDfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  expect(ModuloMappingVerifier::verify(simpleDfg, target, makeSimpleMapping(target)).ok(),
         "single FU mapping is valid");

  const auto chainDfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  const auto chainMapping = makeChainMapping(target);
  const auto beforeVerify = toJson(chainMapping);
  expect(parse(beforeVerify) == chainMapping, "mapping JSON round-trip");
  expect(ModuloMappingVerifier::verify(chainDfg, target, chainMapping).ok(),
         "data chain mapping is valid");

  const auto fanoutDfg = legalize(cgra::ir::fixtures::fanout(), target);
  expect(ModuloMappingVerifier::verify(fanoutDfg, target, makeFanoutMapping(target)).ok(),
         "fanout mapping is valid");
  const auto predicateDfg = legalize(cgra::ir::fixtures::predicateSelectUnsigned(), target);
  expect(ModuloMappingVerifier::verify(predicateDfg, target, makePredicateMapping(target)).ok(),
         "predicate mapping is valid");
  expect(toJson(chainMapping) == beforeVerify, "mapping verifier is read-only");

  const auto loadDfg = legalize(cgra::ir::fixtures::recurrence(), target);
  expect(ModuloMappingVerifier::verify(loadDfg, target, makeLoadMapping(target)).ok(),
         "load and VirtualHold mapping is valid");

  const auto memoryDfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  expect(ModuloMappingVerifier::verify(memoryDfg, target, makeMemoryMapping(target)).ok(),
         "memory ordering mapping is valid");

  const auto coissueDfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  expect(ModuloMappingVerifier::verify(coissueDfg, target, makeCoissueMapping(target)).ok(),
         "FU and LSU co-issue mapping is valid");
}

void testInvalidMappings(const cgra::TargetModel& target) {
  const auto simpleDfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  auto duplicate = makeSimpleMapping(target);
  ModuloMappingTestAccess::duplicatePlacement(duplicate);
  auto report = ModuloMappingVerifier::verify(simpleDfg, target, duplicate);
  expect(report.contains(MappingDiagnosticCode::MMAP_NODE_DUPLICATE_PLACEMENT),
         "duplicate placement is rejected");

  const auto chainDfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  auto wrongSeparation = makeChainMapping(target);
  ModuloMappingTestAccess::setRequiredSeparation(wrongSeparation, 9);
  report = ModuloMappingVerifier::verify(chainDfg, target, wrongSeparation);
  expect(report.contains(MappingDiagnosticCode::MMAP_REQUIRED_SEPARATION_MISMATCH),
         "cached separation is independently recomputed");

  const auto loadDfg = legalize(cgra::ir::fixtures::recurrence(), target);
  const auto loadMapping = makeLoadMapping(target);
  Json json = Json::parse(toJson(loadMapping));
  json["dependences"][0]["transport"]["actions"][0]["elapsed"] = 1;
  const auto earlyLoad = parse(json.dump());
  report = ModuloMappingVerifier::verify(loadDfg, target, earlyLoad);
  expect(report.contains(MappingDiagnosticCode::MMAP_LINK_TIME_REGRESSION) ||
             report.contains(MappingDiagnosticCode::MMAP_LINK_TIME_BEFORE_VALUE_READY) ||
             report.contains(MappingDiagnosticCode::MMAP_REQUIRED_SEPARATION_MISMATCH),
         "load route before output readiness is rejected");

  auto conflict = makeChainMapping(target);
  Json conflictJson = Json::parse(toJson(conflict));
  conflictJson["placements"][1]["tile"] = Json::array({0, 0});
  const auto conflictMapping = parse(conflictJson.dump());
  report = ModuloMappingVerifier::verify(chainDfg, target, conflictMapping);
  expect(report.contains(MappingDiagnosticCode::MMAP_FU_RESOURCE_CONFLICT),
         "same FU resource conflict is rejected");

  const auto fanoutDfg = legalize(cgra::ir::fixtures::fanout(), target);
  auto linkConflictJson = Json::parse(toJson(makeFanoutMapping(target)));
  linkConflictJson["dependences"][1]["transport"]["domain"] = "data";
  linkConflictJson["dependences"][1]["transport"]["actions"][0]["direction"] = "east";
  const auto linkConflict = parse(linkConflictJson.dump());
  report = ModuloMappingVerifier::verify(fanoutDfg, target, linkConflict);
  expect(report.contains(MappingDiagnosticCode::MMAP_DATA_LINK_CONFLICT),
         "same data link conflict is rejected");

  const auto loadStoreDfg = legalize(cgra::ir::fixtures::loadAddStore(), target);
  report =
      ModuloMappingVerifier::verify(loadStoreDfg, target, makeInvalidThenValidRouteMapping(target));
  expect(report.contains(MappingDiagnosticCode::MMAP_HOLD_WRONG_TILE),
         "invalid route is diagnosed");
  expect(!report.contains(MappingDiagnosticCode::MMAP_DATA_LINK_CONFLICT),
         "invalid route does not poison later link reservations");
}

void testTargetMutations(const cgra::TargetModel& canonical) {
  const auto chainDfg = legalize(cgra::ir::fixtures::arithmeticChain(), canonical);
  const auto chainMapping = makeChainMapping(canonical);
  auto hopJson = loadTargetJson();
  hopJson["interconnect"]["hop_latency"] = 2;
  hopJson["parameters"]["mesh_hop_latency"] = 2;
  TemporaryTarget hopFile(hopJson);
  const auto hopTarget = cgra::TargetModel::loadFromFile(hopFile.path());
  auto report = ModuloMappingVerifier::verify(chainDfg, hopTarget, chainMapping);
  expect(report.contains(MappingDiagnosticCode::MMAP_REQUIRED_SEPARATION_MISMATCH),
         "mesh hop latency mutation invalidates cached separation");

  auto loadJson = loadTargetJson();
  loadJson["operations"]["LOAD"]["producer_output_ready_offset"] = 3;
  TemporaryTarget loadFile(loadJson);
  const auto loadTargetModel = cgra::TargetModel::loadFromFile(loadFile.path());
  const auto loadDfg = legalize(cgra::ir::fixtures::recurrence(), loadTargetModel);
  const auto oldTimingMapping = makeLoadMapping(canonical);
  report = ModuloMappingVerifier::verify(loadDfg, loadTargetModel, oldTimingMapping);
  expect(report.contains(MappingDiagnosticCode::MMAP_LINK_TIME_BEFORE_VALUE_READY),
         "LOAD output-ready mutation invalidates early route");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testPositiveMappings(target);
    testInvalidMappings(target);
    testTargetMutations(target);
    std::cout << "CGRA_MODULO_MAPPING_VERIFIER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_MODULO_MAPPING_VERIFIER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
