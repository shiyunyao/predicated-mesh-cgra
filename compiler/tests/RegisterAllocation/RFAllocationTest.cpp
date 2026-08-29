// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloMapping.h"
#include "cgra/RegisterAllocation/PeriodicLifetime.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/RegisterAllocation/StorageRequirementAnalysis.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace cgra::register_allocation;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel target() { return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json"); }

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& model) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, model);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

cgra::mapping::ModuloMapperOptions mapperOptions() {
  cgra::mapping::ModuloMapperOptions options;
  options.maxII = 4;
  options.budget.maxNodeCandidateAttempts = 20000;
  options.budget.maxBacktracks = 10000;
  options.budget.maxRouteSearchCalls = 20000;
  options.budget.perRouteBudget.maxStateExpansions = 10000;
  options.budget.perRouteBudget.maxQueuePushes = 20000;
  return options;
}

std::pair<cgra::target::TargetDFG, cgra::schedule::StagedMapping>
mapAndStage(const cgra::ir::DFG& generic, const cgra::TargetModel& model) {
  const auto dfg = legalize(generic, model);
  const auto mapped = cgra::mapping::ModuloMapper::map(dfg, model, mapperOptions());
  if (!mapped.ok())
    throw std::runtime_error(mapped.format());
  const auto staged = cgra::schedule::StageScheduler::schedule(dfg, model, *mapped.mapping);
  if (!staged.ok())
    throw std::runtime_error(staged.format());
  return {dfg, *staged.mapping};
}

struct SelectStorageCase {
  cgra::target::TargetDFG dfg;
  cgra::schedule::StagedMapping staged;
};

SelectStorageCase makeSelectStorageCase(const cgra::TargetModel& model, bool bothNetwork) {
  cgra::ir::DFGBuilder generic("rf_select_ports");
  const auto predicate = generic.addExternal("predicate", cgra::ir::ValueType::predicate());
  const auto a0 = generic.addExternal("a0", cgra::ir::ValueType::i32());
  const auto a1 = generic.addExternal("a1", cgra::ir::ValueType::i32());
  const auto b0 = generic.addExternal("b0", cgra::ir::ValueType::i32());
  const auto b1 = generic.addExternal("b1", cgra::ir::ValueType::i32());
  const auto lhs = generic.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  const auto rhs = generic.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  const auto select = generic.addNode(
      cgra::ir::Opcode::Select,
      {cgra::ir::ValueType::predicate(), cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
      cgra::ir::ValueType::i32());
  generic.bindExternal(lhs, 0, a0);
  generic.bindExternal(lhs, 1, a1);
  generic.bindExternal(rhs, 0, b0);
  generic.bindExternal(rhs, 1, b1);
  generic.bindExternal(select, 0, predicate);
  generic.addDataEdge(lhs, select, 1);
  generic.addDataEdge(rhs, select, 2);
  const auto dfg = legalize(generic.finish(), model);

  const cgra::mapping::TileCoord center{1, 1};
  const cgra::mapping::TileCoord west{1, 0};
  const cgra::mapping::TileCoord east{1, 2};
  cgra::mapping::ModuloMappingBuilder builder(dfg, 2);
  builder.place(lhs, bothNetwork ? west : center, cgra::mapping::ModuloSlot(bothNetwork ? 0 : 1));
  builder.place(rhs, bothNetwork ? east : west, cgra::mapping::ModuloSlot(0));
  builder.place(select, center, cgra::mapping::ModuloSlot(0));
  if (bothNetwork) {
    builder.setTransport(0, cgra::mapping::TransportPlan{
                                0,
                                cgra::mapping::NetworkDomain::Data,
                                {cgra::mapping::LinkStep{cgra::mapping::NetworkDomain::Data, west,
                                                         cgra::mapping::Direction::East, 0}},
                                1});
    builder.setTransport(1, cgra::mapping::TransportPlan{
                                1,
                                cgra::mapping::NetworkDomain::Data,
                                {cgra::mapping::LinkStep{cgra::mapping::NetworkDomain::Data, east,
                                                         cgra::mapping::Direction::West, 0}},
                                1});
  } else {
    builder.setTransport(
        0, cgra::mapping::TransportPlan{
               0,
               cgra::mapping::NetworkDomain::Data,
               {cgra::mapping::VirtualHold{cgra::mapping::NetworkDomain::Data, center, 0, 1}},
               1});
    builder.setTransport(1, cgra::mapping::TransportPlan{
                                1,
                                cgra::mapping::NetworkDomain::Data,
                                {cgra::mapping::LinkStep{cgra::mapping::NetworkDomain::Data, west,
                                                         cgra::mapping::Direction::East, 0}},
                                1});
  }
  const auto modulo = builder.finish();
  const auto stage = cgra::schedule::StageScheduler::schedule(dfg, model, modulo);
  if (!stage.ok())
    throw std::runtime_error(stage.format());
  return {dfg, *stage.mapping};
}

SelectStorageCase makePredicateStorageCase(const cgra::TargetModel& model) {
  auto targetNode = [&](cgra::target::TargetNodeId id,
                        std::string operation) -> cgra::target::TargetNode {
    const auto& desc = model.operation(operation);
    std::vector<cgra::ir::ValueType> operands;
    for (const auto& operand : desc.operands)
      operands.push_back(operand.role == cgra::TargetOperandRole::Predicate
                             ? cgra::ir::ValueType::predicate()
                             : cgra::ir::ValueType::i32());
    return {id,
            std::move(operation),
            desc.executionClass,
            desc.resultType,
            std::move(operands),
            desc.issueOccupancy,
            desc.resultLatency,
            {id},
            desc.producerOutputReadyOffset,
            desc.accessWidthBits};
  };
  cgra::target::TargetDFGBuilder builder("rf_predicate_ports", std::string(model.name()));
  for (std::uint32_t id = 0; id < 4; ++id)
    builder.addExternal({id, cgra::ir::ValueType::i32(), "input" + std::to_string(id)});
  builder.addNode(targetNode(0, "CMP_ULT"));
  builder.addNode(targetNode(1, "CMP_ULT"));
  builder.addNode(targetNode(2, "PAND"));
  builder.addBinding({0, 0, cgra::ir::ExternalValueRef{0}});
  builder.addBinding({0, 1, cgra::ir::ExternalValueRef{1}});
  builder.addBinding({1, 0, cgra::ir::ExternalValueRef{2}});
  builder.addBinding({1, 1, cgra::ir::ExternalValueRef{3}});
  builder.addEdge({0, 0, 2, 0, cgra::ir::PredicateEdgeInfo{0}});
  builder.addEdge({1, 1, 2, 0, cgra::ir::PredicateEdgeInfo{1}});
  // PAND's two predicate operands are distinct; use one same-tile FU result
  // and one incoming network value to exercise W0/W1 source asymmetry.
  const auto dfg = builder.finish();
  cgra::mapping::ModuloMappingBuilder mapping(dfg, 2);
  mapping.place(0, {1, 1}, cgra::mapping::ModuloSlot(1));
  mapping.place(1, {1, 0}, cgra::mapping::ModuloSlot(0));
  mapping.place(2, {1, 1}, cgra::mapping::ModuloSlot(0));
  mapping.setTransport(
      0, cgra::mapping::TransportPlan{
             0,
             cgra::mapping::NetworkDomain::Predicate,
             {cgra::mapping::VirtualHold{cgra::mapping::NetworkDomain::Predicate, {1, 1}, 0, 1}},
             1});
  mapping.setTransport(
      1,
      cgra::mapping::TransportPlan{
          1,
          cgra::mapping::NetworkDomain::Predicate,
          {cgra::mapping::LinkStep{
              cgra::mapping::NetworkDomain::Predicate, {1, 0}, cgra::mapping::Direction::East, 0}},
          1});
  const auto staged = cgra::schedule::StageScheduler::schedule(dfg, model, mapping.finish());
  if (!staged.ok())
    throw std::runtime_error(staged.format());
  return {dfg, *staged.mapping};
}

void testExactRFPortRegressions(const cgra::TargetModel& model) {
  const auto selectCase = makeSelectStorageCase(model, false);
  const auto requirements =
      StorageRequirementAnalysis::analyze(selectCase.dfg, model, selectCase.staged);
  expect(requirements.ok() && requirements.requirements->segments().size() == 2,
         "SELECT case derives two DataRF storage segments");
  const auto allocation = RFAllocator::allocate(selectCase.dfg, model, selectCase.staged);
  if (!allocation.ok())
    throw std::runtime_error(
        allocation.format() + " stages=" + std::to_string(selectCase.staged.maxStage()) +
        " req=" + std::to_string(requirements.requirements->segments().size()) +
        " w=" + std::to_string(requirements.requirements->segment(0).writeTime) +
        " r=" + std::to_string(requirements.requirements->segment(0).readTime));
  expect(RFAllocationVerifier::verify(selectCase.dfg, model, *allocation.mapping).ok(),
         "valid SELECT RF allocation passes the independent verifier");
  std::vector<std::uint32_t> readPorts;
  std::vector<std::uint32_t> writePorts;
  for (const auto& item : allocation.mapping->allocations()) {
    readPorts.push_back(item.readPort);
    writePorts.push_back(item.writePort);
  }
  std::ranges::sort(readPorts);
  std::ranges::sort(writePorts);
  expect(readPorts == std::vector<std::uint32_t>({0, 1}),
         "SELECT data operands use the two physical DataRF read ports");
  expect(writePorts == std::vector<std::uint32_t>({0, 1}),
         "FU-result and network writes use W0 and W1 respectively");
  for (const auto& item : allocation.mapping->allocations()) {
    const auto& segment = allocation.mapping->storageRequirements().segment(item.segment);
    expect(item.writePort == (segment.edge == 0 ? 0U : 1U),
           "SELECT storage provenance selects the source-compatible write port");
  }

  const auto networkOnly = makeSelectStorageCase(model, true);
  const auto collision = RFAllocator::allocate(networkOnly.dfg, model, networkOnly.staged);
  if (collision.status != RFAllocationStatus::WritePortConflict)
    throw std::runtime_error("two W1-only writes status: " + collision.format());

  auto corruptedJson =
      nlohmann::json::parse(RFAllocatedMappingSerialization::toJson(*allocation.mapping));
  for (auto& segment : corruptedJson["storage_segments"])
    if (segment["write_port"].get<unsigned>() == 1)
      segment["write_port"] = 0;
  const auto corrupted = RFAllocatedMappingSerialization::parse(corruptedJson.dump());
  expect(!RFAllocationVerifier::verify(selectCase.dfg, model, corrupted).ok(),
         "corrupt RF port assignment is rejected by the independent verifier");
}

void testLsuLoadWritePort(const cgra::TargetModel& model) {
  const auto dfg = legalize(cgra::ir::fixtures::loadAddStore(), model);
  cgra::mapping::ModuloMappingBuilder builder(dfg, 3);
  builder.place(0, {0, 0}, cgra::mapping::ModuloSlot(0));
  builder.place(1, {1, 1}, cgra::mapping::ModuloSlot(0));
  builder.place(2, {1, 0}, cgra::mapping::ModuloSlot(0));
  builder.setTransport(
      0, cgra::mapping::TransportPlan{
             0,
             cgra::mapping::NetworkDomain::Data,
             {cgra::mapping::VirtualHold{cgra::mapping::NetworkDomain::Data, {0, 0}, 2, 3},
              cgra::mapping::LinkStep{
                  cgra::mapping::NetworkDomain::Data, {0, 0}, cgra::mapping::Direction::South, 3},
              cgra::mapping::LinkStep{
                  cgra::mapping::NetworkDomain::Data, {1, 0}, cgra::mapping::Direction::East, 4}},
             5});
  builder.setTransport(
      1, cgra::mapping::TransportPlan{
             1,
             cgra::mapping::NetworkDomain::Data,
             {cgra::mapping::LinkStep{
                 cgra::mapping::NetworkDomain::Data, {1, 1}, cgra::mapping::Direction::West, 0}},
             1});
  const auto stage = cgra::schedule::StageScheduler::schedule(dfg, model, builder.finish());
  if (!stage.ok())
    throw std::runtime_error(stage.format());
  const auto& staged = *stage.mapping;
  const auto allocation = RFAllocator::allocate(dfg, model, staged);
  if (!allocation.ok())
    throw std::runtime_error(allocation.format());
  bool found = false;
  for (const auto& item : allocation.mapping->allocations()) {
    const auto& segment = allocation.mapping->storageRequirements().segment(item.segment);
    for (const auto& origin : segment.origins)
      if (origin.kind == StorageOriginKind::ExplicitVirtualHold && segment.edge == 0)
        found = found || item.writePort == 1;
  }
  expect(found, "LSU load-result storage write uses the W1-compatible port");
}

void testPredicateRFPortRegressions(const cgra::TargetModel& model) {
  const auto predicateCase = makePredicateStorageCase(model);
  const auto allocation = RFAllocator::allocate(predicateCase.dfg, model, predicateCase.staged);
  if (!allocation.ok())
    throw std::runtime_error(allocation.format());
  expect(RFAllocationVerifier::verify(predicateCase.dfg, model, *allocation.mapping).ok(),
         "valid predicate RF allocation passes the independent verifier");
  std::vector<std::uint32_t> writePorts;
  for (const auto& item : allocation.mapping->allocations())
    writePorts.push_back(item.writePort);
  std::ranges::sort(writePorts);
  expect(writePorts == std::vector<std::uint32_t>({0, 1}),
         "FU predicate and network predicate writes use W0 and W1");
  for (const auto& item : allocation.mapping->allocations()) {
    const auto& segment = allocation.mapping->storageRequirements().segment(item.segment);
    expect(item.writePort == (segment.edge == 0 ? 0U : 1U),
           "predicate storage provenance selects the source-compatible write port");
  }

  auto corruptedJson =
      nlohmann::json::parse(RFAllocatedMappingSerialization::toJson(*allocation.mapping));
  for (auto& segment : corruptedJson["storage_segments"])
    if (segment["write_port"].get<unsigned>() == 1)
      segment["write_port"] = 0;
  const auto corrupted = RFAllocatedMappingSerialization::parse(corruptedJson.dump());
  expect(!RFAllocationVerifier::verify(predicateCase.dfg, model, corrupted).ok(),
         "predicate network W1 corruption is rejected by the verifier");
}

void testCanonicalNoStorage(const cgra::TargetModel& model) {
  const auto [dfg, staged] = mapAndStage(cgra::ir::fixtures::simpleAdd(), model);
  const auto requirements = StorageRequirementAnalysis::analyze(dfg, model, staged);
  expect(requirements.ok(), "simple add storage analysis succeeds");
  expect(requirements.requirements->segments().empty(), "direct route has no RF storage");
  const auto allocation = RFAllocator::allocate(dfg, model, staged);
  expect(allocation.ok(), "simple add RF allocation succeeds");
  expect(RFAllocationVerifier::verify(dfg, model, *allocation.mapping).ok(),
         "simple add allocation passes independent verifier");
}

void testVirtualHoldAndRecurrence(const cgra::TargetModel& model) {
  const auto [dfg, staged] = mapAndStage(cgra::ir::fixtures::recurrence(), model);
  const auto requirements = StorageRequirementAnalysis::analyze(dfg, model, staged);
  expect(requirements.ok() && !requirements.requirements->segments().empty(),
         "recurrence derives explicit storage");
  const auto allocation = RFAllocator::allocate(dfg, model, staged);
  expect(allocation.status == RFAllocationStatus::FixedRegisterSelfOverlap,
         "canonical forbidden same-address policy rejects periodic recurrence storage");
  expect(std::ranges::any_of(allocation.diagnostics,
                             [](const auto& diagnostic) {
                               return diagnostic.code ==
                                      RFAllocationDiagnosticCode::RFA_FIXED_REGISTER_SELF_OVERLAP;
                             }),
         "fixed-RF periodic rejection reports the exact self-overlap diagnostic");

  RFAllocationOptions rotatingOptions;
  rotatingOptions.enableSoftwareRotation = true;
  const auto rotating = RFAllocator::allocate(dfg, model, staged, rotatingOptions);
  if (!rotating.ok())
    throw std::runtime_error("software-rotation recurrence allocation: " + rotating.format());
  const auto& rotatingAllocation = rotating.mapping->allocationFor(0);
  expect(rotatingAllocation.family.phaseCount == 2,
         "duration equal to II uses two software-rotation phases");
  expect(rotating.mapping->registerFor(0, 0) != rotating.mapping->registerFor(0, 1),
         "successive logical iterations use distinct phase registers");
  expect(rotating.mapping->registerFor(0, -1) == rotating.mapping->registerFor(0, 1),
         "negative iterations use mathematical floor-mod phase selection");
  expect(RFAllocationVerifier::verify(dfg, model, *rotating.mapping).ok(),
         "phase-expanded allocation passes the independent verifier");

  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["data_rf"]["same_cycle_read_write_same_address"] = "read_old_then_write_new";
  const auto path = std::filesystem::temp_directory_path() / "cgra-rf-read-old.json";
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  const auto readOldTarget = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  const auto [readOldDfg, readOldStaged] =
      mapAndStage(cgra::ir::fixtures::recurrence(), readOldTarget);
  const auto readOldAllocation = RFAllocator::allocate(readOldDfg, readOldTarget, readOldStaged);
  if (!readOldAllocation.ok())
    throw std::runtime_error("read-old recurrence allocation: " + readOldAllocation.format());
  expect(readOldAllocation.mapping->registerForVirtualHold(1, 0).has_value(),
         "VirtualHold provenance resolves to physical RF");
  const auto roundTrip = RFAllocatedMappingSerialization::parse(
      RFAllocatedMappingSerialization::toJson(*readOldAllocation.mapping));
  expect(roundTrip == *readOldAllocation.mapping,
         "RF allocation debug serialization preserves the complete allocation");
}

void testPeriodicHelpers() {
  StorageSegment shortSegment{0, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 4, {}};
  expect(!fixedRegisterSelfOverlaps(shortSegment, 4), "short lifetime is self-compatible");
  StorageSegment longSegment{1, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 8, {}};
  expect(fixedRegisterSelfOverlaps(longSegment, 4), "long lifetime self-overlaps");
  StorageSegment boundary{2, 0, {0, 0}, cgra::RegisterBankDomain::Data, 0, 2, {}};
  StorageSegment wrapped{3, 0, {0, 0}, cgra::RegisterBankDomain::Data, 3, 4, {}};
  expect(periodicLifetimesConflict(boundary, wrapped, 4),
         "periodic modulo-boundary conflict is detected");
  expect(!periodicLifetimesConflict(boundary, wrapped, 4,
                                    cgra::SameAddressReadWritePolicy::ReadOldThenWriteNew),
         "read-old/write-new allows boundary-only reuse");
}

void testTargetRFMutation(const cgra::TargetModel& canonical) {
  (void)canonical;
  std::ifstream input(Root / "target/cgra_v2.json");
  nlohmann::json json;
  input >> json;
  json["data_rf"]["read_ports"] = 1;
  json["data_rf"]["write_ports"] = 1;
  json["data_rf"]["write_ports_detail"].erase("W1");
  const auto path = std::filesystem::temp_directory_path() / "cgra-rf-mutation.json";
  std::ofstream output(path);
  output << json.dump(2) << '\n';
  output.close();
  const auto mutated = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  expect(mutated.dataRF().readPorts == 1 && mutated.dataRF().writePorts == 1,
         "RF port mutation is target-driven");
}
} // namespace

int main() {
  try {
    const auto model = target();
    testCanonicalNoStorage(model);
    testVirtualHoldAndRecurrence(model);
    testExactRFPortRegressions(model);
    testLsuLoadWritePort(model);
    testPredicateRFPortRegressions(model);
    testPeriodicHelpers();
    testTargetRFMutation(model);
    std::cout << "RF allocation tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "RF allocation tests failed: " << error.what() << '\n';
    return 1;
  }
}
