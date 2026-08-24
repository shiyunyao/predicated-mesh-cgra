// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ResourceReservation.h"
#include "cgra/Target/TargetDFGVerifier.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

using Json = nlohmann::json;
using namespace cgra::mapping;
const std::filesystem::path RepositoryRoot = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(RepositoryRoot / "target/cgra_v2.json");
}

Json loadTargetJson() {
  std::ifstream stream(RepositoryRoot / "target/cgra_v2.json");
  Json json;
  stream >> json;
  return json;
}

class TemporaryTarget {
public:
  explicit TemporaryTarget(const Json& json) {
    static unsigned serial = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("cgra-mapping-test-target-" + std::to_string(serial++) + ".json");
    std::ofstream stream(path_);
    stream << json.dump(2) << '\n';
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

void testModuloTimeAndTopology(const cgra::TargetModel& target) {
  ModuloTimeDomain time(4);
  expect(time.normalize(0).value() == 0 && time.normalize(7).value() == 3, "modulo normalization");
  expect(time.advance(ModuloSlot(3), 1).value() == 0 && time.advance(ModuloSlot(3), 5).value() == 0,
         "modulo advance wrap");
  expect(time.advance(ModuloSlot(3), std::numeric_limits<std::uint64_t>::max()).value() == 2,
         "modulo advance handles large elapsed values");
  bool rejected = false;
  try {
    ModuloTimeDomain(0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "II zero must be rejected");

  const TileCoord corner{0, 0};
  expect(!neighbor(corner, Direction::North, 4, 4), "corner has no north link");
  expect(!neighbor({4, 0}, Direction::South, 4, 4), "out-of-range tile has no neighbor");
  expect(neighbor(corner, Direction::East, target) == TileCoord{0, 1},
         "target-based neighbor uses target dimensions");
  expect(neighbor(corner, Direction::East, 4, 4) == TileCoord{0, 1}, "east neighbor");
  expect(opposite(Direction::East) == Direction::West, "opposite direction");

  ModuloResourceModel model(target, 4);
  const auto stats = model.stats();
  expect(stats.fuResources == 4 * 16 && stats.lsuResources == 4 * 4,
         "FU and LSU resources per modulo slot");
  expect(stats.dataLinkResources == 4 * 48 && stats.predicateLinkResources == 4 * 48,
         "directional network resource count");
  expect(stats.totalResources == stats.fuResources + stats.lsuResources + stats.dataLinkResources +
                                     stats.predicateLinkResources,
         "resource statistics total");
  for (ResourceId id = 0; id < model.resourceCount(); ++id) {
    const auto& resource = model.resource(id);
    const auto roundTrip = std::visit(
        [&](const auto& value) -> ResourceId {
          using Resource = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Resource, FUResource>)
            return model.fuResource(value.tile, value.slot);
          else if constexpr (std::is_same_v<Resource, LSUResource>)
            return model.lsuResource(value.tile, value.slot).value();
          else
            return model.linkResource(value.domain, value.source, value.direction, value.slot)
                .value();
        },
        resource);
    expect(roundTrip == id, "semantic resource lookup round-trips to stable ResourceId");
  }

  ModuloResourceModel modelII8(target, 8);
  expect(modelII8.stats().totalResources == 2 * stats.totalResources,
         "resource count scales with II only");
  ModuloResourceModel modelII1(target, 1);
  ModuloResourceModel modelII2(target, 2);
  ModuloResourceModel modelII7(target, 7);
  expect(modelII1.stats().totalResources * 4 == stats.totalResources,
         "II=1 has one resource layer");
  expect(modelII2.stats().totalResources * 2 == stats.totalResources,
         "II=2 has two resource layers");
  expect(modelII7.stats().totalResources == (stats.totalResources / 4) * 7,
         "II=7 has seven resource layers");
  expect(!model.linkResource(NetworkDomain::Data, corner, Direction::North, ModuloSlot(0)),
         "border link absent");
  expect(model.linkResource(NetworkDomain::Data, corner, Direction::East, ModuloSlot(0)) !=
             model.linkResource(NetworkDomain::Predicate, corner, Direction::East, ModuloSlot(0)),
         "data and predicate links are independent resources");
}

void testFootprintsAndReservations(const cgra::TargetModel& target) {
  const auto generic = cgra::ir::fixtures::simpleAdd();
  const auto dfg = legalize(generic, target);
  ModuloResourceModel model(target, 4);
  const auto addFootprint = model.operationFootprint(dfg.node(0), {0, 0}, ModuloSlot(3));
  expect(addFootprint.size() == 1 &&
             std::get<FUResource>(model.resource(addFootprint.front())).slot.value() == 3,
         "FU footprint uses issue slot");

  const auto loadDfg = legalize(cgra::ir::fixtures::recurrence(), target);
  const auto loadFootprint = model.operationFootprint(loadDfg.node(0), {0, 0}, ModuloSlot(3));
  expect(loadFootprint.size() == 1 &&
             std::get<LSUResource>(model.resource(loadFootprint.front())).slot.value() == 3,
         "LSU footprint uses one issue resource");
  ResourceReservationTable reservations(model);
  expect(reservations.reserve(addFootprint, {ReservationOwnerKind::Node, 0}),
         "first resource reservation");
  expect(!reservations.reserve(addFootprint, {ReservationOwnerKind::Node, 1}),
         "same resource conflicts");
  expect(reservations.reserve(loadFootprint, {ReservationOwnerKind::Node, 1}),
         "FU and LSU co-issue is legal");

  std::vector<ResourceId> fourLsuResources;
  for (std::uint32_t row = 0; row < 4; ++row)
    fourLsuResources.push_back(model.lsuResource({row, 0}, ModuloSlot(1)).value());
  expect(reservations.reserve(fourLsuResources, {ReservationOwnerKind::Node, 2}),
         "four statically assigned LSU ports issue together");

  const auto dataLink =
      model.linkResource(NetworkDomain::Data, {1, 1}, Direction::East, ModuloSlot(0));
  const auto predicateLink =
      model.linkResource(NetworkDomain::Predicate, {1, 1}, Direction::East, ModuloSlot(0));
  const auto northLink =
      model.linkResource(NetworkDomain::Data, {1, 1}, Direction::North, ModuloSlot(0));
  const std::vector<ResourceId> dataResources{dataLink.value()};
  const std::vector<ResourceId> predicateResources{predicateLink.value()};
  const std::vector<ResourceId> duplicateResources{dataLink.value(), dataLink.value()};
  expect(dataLink && predicateLink &&
             reservations.reserve(dataResources, {ReservationOwnerKind::Edge, 4}) &&
             reservations.reserve(predicateResources, {ReservationOwnerKind::Edge, 5}),
         "data and predicate link reservations are independent");
  expect(northLink && reservations.reserve(std::vector<ResourceId>{northLink.value()},
                                           {ReservationOwnerKind::Edge, 3}),
         "different output directions are independent");
  expect(!reservations.reserve(duplicateResources, {ReservationOwnerKind::Edge, 6}),
         "duplicate resource in one transaction fails atomically");
  const std::vector<ResourceId> deltaResources{model.fuResource({0, 1}, ModuloSlot(0))};
  const auto delta = reservations.tryReserve(deltaResources, {ReservationOwnerKind::Node, 9});
  expect(delta.has_value(), "reservation delta");
  reservations.undo(*delta);
  expect(reservations.isFree(model.fuResource({0, 1}, ModuloSlot(0))), "undo restores state");
  reservations.release(addFootprint, {ReservationOwnerKind::Node, 0});
  reservations.release(loadFootprint, {ReservationOwnerKind::Node, 1});
  reservations.release(fourLsuResources, {ReservationOwnerKind::Node, 2});
}

void testOccupancyMutation(const cgra::TargetModel& canonical) {
  auto json = loadTargetJson();
  json["operations"]["ADD"]["issue_occupancy"] = 2;
  TemporaryTarget file(json);
  const auto target = cgra::TargetModel::loadFromFile(file.path());
  const auto dfg = legalize(cgra::ir::fixtures::simpleAdd(), target);
  ModuloResourceModel model(target, 4);
  const auto wrapped = model.operationFootprint(dfg.node(0), {0, 0}, ModuloSlot(3));
  expect(wrapped.size() == 2 && std::get<FUResource>(model.resource(wrapped[1])).slot.value() == 0,
         "occupancy wraps modulo II");

  auto iiOne = ModuloResourceModel(target, 1);
  bool rejected = false;
  try {
    iiOne.operationFootprint(dfg.node(0), {0, 0}, ModuloSlot(0));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "self-overlapping footprint is rejected");
  expect(canonical.operation("ADD").producerOutputReadyOffset == 0,
         "FU output readiness comes from target contract");
  expect(canonical.operation("LOAD").producerOutputReadyOffset == 2,
         "LOAD output readiness comes from target contract");
  expect(canonical.memoryDependenceSeparation(cgra::ir::MemoryDepKind::RAW) == 1,
         "memory separation comes from target contract");

  auto offsetJson = loadTargetJson();
  offsetJson["operations"]["LOAD"]["producer_output_ready_offset"] = 5;
  {
    TemporaryTarget offsetFile(offsetJson);
    const auto offsetTarget = cgra::TargetModel::loadFromFile(offsetFile.path());
    expect(offsetTarget.operation("LOAD").producerOutputReadyOffset == 5,
           "LOAD output readiness mutation is observable");
  }
}

void testTileCapabilities() {
  auto json = loadTargetJson();
  json["tile_capabilities"]["overrides"] =
      Json::array({{{"row", 0}, {"col", 0}, {"operations", Json::array({"ADD"})}}});
  TemporaryTarget file(json);
  const auto target = cgra::TargetModel::loadFromFile(file.path());
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  const auto& mul = dfg.node(1);
  ModuloResourceModel model(target, 1);
  expect(!model.supportsOperation({0, 0}, mul), "tile capability override rejects MUL");
  expect(model.supportsOperation({0, 1}, mul), "default tile capability permits MUL");
}

void testMappingAndSerialization(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::recurrence(), target);
  ModuloMappingBuilder builder(dfg, 2);
  for (const auto& node : dfg.nodes())
    builder.place(node.id, {0, 0}, ModuloSlot(node.id % 2));
  for (const auto& edge : dfg.edges()) {
    if (edge.kind() == cgra::ir::Edge::Kind::Memory) {
      builder.setMemorySeparation(edge.id,
                                  target.memoryDependenceSeparation(
                                      std::get<cgra::ir::MemoryEdgeInfo>(edge.info).dependence));
    } else {
      const auto domain = edge.kind() == cgra::ir::Edge::Kind::Predicate ? NetworkDomain::Predicate
                                                                         : NetworkDomain::Data;
      const auto firstElapsed = edge.id == 0 ? 2U : 0U;
      TransportPlan transport{edge.id,
                              domain,
                              {LinkStep{domain, {0, 0}, Direction::East, firstElapsed},
                               LinkStep{domain, {0, 1}, Direction::East, 1},
                               LinkStep{domain, {0, 2}, Direction::East, 3},
                               VirtualHold{domain, {0, 3}, 3, 4}},
                              edge.id == 0 ? 3U : 4U};
      builder.setTransport(edge.id, std::move(transport));
    }
  }
  const auto mapping = builder.finish();
  expect(mapping.placement(0).issueSlot.value() == 0, "placement lookup");
  const auto json = toJson(mapping);
  expect(json.find("stage") == std::string::npos && json.find("absolute") == std::string::npos,
         "mapping has no stage or absolute time");
  expect(parse(json) == mapping, "mapping JSON round-trip");
  auto mismatchedKind = Json::parse(json);
  mismatchedKind["dependences"][0]["kind"] = "predicate";
  bool mismatchRejected = false;
  try {
    parse(mismatchedKind.dump());
  } catch (const std::invalid_argument&) {
    mismatchRejected = true;
  }
  expect(mismatchRejected, "mapping parser rejects kind/domain mismatch");
  expect(dump(mapping).find("ModuloMapping") != std::string::npos, "mapping debug dump");

  const auto memoryDfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  ModuloMappingBuilder memoryBuilder(memoryDfg, 4);
  for (const auto& node : memoryDfg.nodes())
    memoryBuilder.place(node.id, {node.id, 0}, ModuloSlot(0));
  const auto& edge = memoryDfg.edges().front();
  memoryBuilder.setMemorySeparation(
      edge.id,
      target.memoryDependenceSeparation(std::get<cgra::ir::MemoryEdgeInfo>(edge.info).dependence));
  const auto memoryMapping = memoryBuilder.finish();
  expect(!memoryMapping.dependence(edge.id).transport, "memory edge has no transport route");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testModuloTimeAndTopology(target);
    testFootprintsAndReservations(target);
    testOccupancyMutation(target);
    testTileCapabilities();
    testMappingAndSerialization(target);
    std::cout << "CGRA_MODULO_MAPPING_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_MODULO_MAPPING_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
