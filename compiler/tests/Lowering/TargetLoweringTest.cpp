// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/Lowering/ConstantAllocator.h"
#include "cgra/Lowering/TargetLowering.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/Schedule/MaterializedScheduleVerifier.h"
#include "cgra/Schedule/ScheduleMaterializer.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel target() { return cgra::TargetModel::loadFromFile(Root / "target/cgra_v2.json"); }

cgra::mapping::ModuloMapperOptions mapperOptions() {
  cgra::mapping::ModuloMapperOptions options;
  options.maxII = 4;
  options.budget.maxNodeCandidateAttempts = 20'000;
  options.budget.maxBacktracks = 10'000;
  options.budget.maxRouteSearchCalls = 20'000;
  options.budget.perRouteBudget.maxStateExpansions = 10'000;
  options.budget.perRouteBudget.maxQueuePushes = 20'000;
  return options;
}

cgra::ir::DFG constantAdd() {
  cgra::ir::DFGBuilder builder("constant_add");
  const auto lhs = builder.addConstant(cgra::ir::ValueType::i32(), 7);
  const auto rhs = lhs;
  const auto add = builder.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  builder.bindConstant(add, 0, lhs);
  builder.bindConstant(add, 1, rhs);
  return builder.finish();
}

cgra::ir::DFG sparseConstantAdd() {
  cgra::ir::DFGBuilder builder("sparse_constant_add");
  const auto lhs = builder.importConstant({100, cgra::ir::ValueType::i32(), 7});
  const auto rhs = builder.importConstant({500, cgra::ir::ValueType::i32(), 11});
  const auto add = builder.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  builder.bindConstant(add, 0, lhs);
  builder.bindConstant(add, 1, rhs);
  return builder.finish();
}

cgra::ir::DFG overfullConstantAdd() {
  cgra::ir::DFGBuilder builder("overfull_constant_add");
  std::vector<cgra::ir::ConstantId> constants;
  for (std::uint32_t id = 0; id < 17; ++id)
    constants.push_back(builder.importConstant({id, cgra::ir::ValueType::i32(), id + 1}));
  const auto add = builder.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  builder.bindConstant(add, 0, constants[0]);
  builder.bindConstant(add, 1, constants[1]);
  return builder.finish();
}

void testConstantAllocationDecouplesSemanticIds(const cgra::TargetModel& model) {
  const auto legal = cgra::target::TargetLegalizer::legalize(sparseConstantAdd(), model);
  expect(legal.ok(), "sparse constants legalize");
  const auto image = cgra::lowering::ConstantAllocator::allocate(*legal.dfg, model);
  expect(image.entries.size() == 2, "distinct sparse constants are allocated");
  expect(image.address(100) == 0 && image.address(500) == 1,
         "semantic constant IDs are not physical addresses");
}

void testConstantCapacityFailure(const cgra::TargetModel& model) {
  const auto legal = cgra::target::TargetLegalizer::legalize(overfullConstantAdd(), model);
  expect(legal.ok(), "overfull constants legalize before allocation");
  bool failed = false;
  try {
    (void)cgra::lowering::ConstantAllocator::allocate(*legal.dfg, model);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("capacity exceeded") != std::string::npos;
  }
  expect(failed, "constant capacity failure is explicit and does not alias values");
}

void testDescriptorAndIdleRoundTrip(const cgra::TargetModel& model) {
  const auto& add = model.operation("ADD");
  expect(add.operandSinks.size() == 2, "ADD has two typed lowering sinks");
  expect(add.operandSinks[0].second == cgra::TargetControlSink::FuDataA,
         "ADD operand zero is FU source A");
  expect(add.operandSinks[1].second == cgra::TargetControlSink::FuDataB,
         "ADD operand one is FU source B");
  expect(add.resultSource == cgra::TargetResultSource::FuDataResult,
         "ADD result source is FU data result");
  const cgra::TileControl idle;
  expect(cgra::encode(cgra::decode(cgra::encode(idle, model), model), model) ==
             cgra::encode(idle, model),
         "canonical idle control round-trips through target encoding");
}

void testConstantLoweringAndManifest(const cgra::TargetModel& model) {
  const auto legal = cgra::target::TargetLegalizer::legalize(constantAdd(), model);
  expect(legal.ok(), "constant ADD legalizes");
  const auto mapped = cgra::mapping::ModuloMapper::map(*legal.dfg, model, mapperOptions());
  expect(mapped.ok(), "constant ADD maps");
  const auto staged = cgra::schedule::StageScheduler::schedule(*legal.dfg, model, *mapped.mapping);
  expect(staged.ok(), "constant ADD stage schedules");
  const auto allocated =
      cgra::register_allocation::RFAllocator::allocate(*legal.dfg, model, *staged.mapping);
  expect(allocated.ok(), "constant ADD RF allocates");
  cgra::schedule::ScheduleMaterializationRequest request;
  request.tripCount = 4;
  const auto materialized = cgra::schedule::ScheduleMaterializer::materialize(
      *legal.dfg, model, *allocated.mapping, request);
  expect(materialized.ok(), "constant ADD materializes");
  const auto lowered = cgra::lowering::TargetLowering::lower(
      *legal.dfg, model, *allocated.mapping, *materialized.schedule,
      cgra::lowering::TargetLoweringOptions{
          "constant-add", (Root / "target/cgra_v2.json").string(), "test", {}, {}});
  if (!lowered.ok())
    throw std::runtime_error(lowered.diagnostics.front().message);
  expect(lowered.controls->kernel().body.size() == lowered.controls->ii(),
         "target control kernel has exactly II cycles");
  expect(lowered.encoded->kernel.body.size() == lowered.controls->ii(),
         "encoded kernel preserves compact II shape");
  const auto json = nlohmann::json::parse(lowered.manifest->json);
  expect(json.at("schema") == "cgra.program_manifest.v1", "manifest schema is v1");
  expect(json.at("loop").at("trip_count") == 4, "manifest preserves trip count");
  expect(json.at("program").at("tiles").size() == model.array().rows * model.array().cols,
         "manifest contains one image per target tile");

  const auto path = std::filesystem::temp_directory_path() / "cgra-target-lowering-manifest.json";
  std::ofstream output(path);
  output << lowered.manifest->json << '\n';
  output.close();
  const auto validator = Root / "tools/validate_program.py";
  const auto command = "python3 " + validator.string() + " " + path.string();
  expect(std::system(command.c_str()) == 0, "existing manifest validator accepts generated output");
  std::filesystem::remove(path);
}

void testExternalProviderIsExplicitFailure(const cgra::TargetModel& model) {
  cgra::target::TargetDFG dfg;
  const auto legal =
      cgra::target::TargetLegalizer::legalize(cgra::ir::fixtures::simpleAdd(), model);
  expect(legal.ok(), "simple ADD legalizes");
  dfg = *legal.dfg;
  const auto mapped = cgra::mapping::ModuloMapper::map(dfg, model, mapperOptions());
  expect(mapped.ok(), "simple ADD maps");
  const auto staged = cgra::schedule::StageScheduler::schedule(dfg, model, *mapped.mapping);
  expect(staged.ok(), "simple ADD stage schedules");
  const auto allocated =
      cgra::register_allocation::RFAllocator::allocate(dfg, model, *staged.mapping);
  expect(allocated.ok(), "simple ADD RF allocates");
  cgra::schedule::ScheduleMaterializationRequest request;
  request.tripCount = 1;
  const auto materialized =
      cgra::schedule::ScheduleMaterializer::materialize(dfg, model, *allocated.mapping, request);
  expect(materialized.ok(), "simple ADD materializes");
  const auto result =
      cgra::lowering::TargetLowering::lower(dfg, model, *allocated.mapping, *materialized.schedule);
  expect(result.status == cgra::lowering::TargetLoweringStatus::UnsupportedExternalProvider,
         "unsupported external provider is classified explicitly");
}
} // namespace

int main() {
  try {
    const auto model = target();
    testDescriptorAndIdleRoundTrip(model);
    testConstantAllocationDecouplesSemanticIds(model);
    testConstantCapacityFailure(model);
    testConstantLoweringAndManifest(model);
    testExternalProviderIsExplicitFailure(model);
    std::cout << "target lowering tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "target lowering tests failed: " << error.what() << '\n';
    return 1;
  }
}
