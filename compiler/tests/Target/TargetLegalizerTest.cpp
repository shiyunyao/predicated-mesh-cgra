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
  expect(cmpResult.toJson().find("TLEG_UNSUPPORTED_ICMP_PREDICATE") != std::string::npos,
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
  auto& dataOps = targetJson["ops"]["data"];
  dataOps.erase(std::remove(dataOps.begin(), dataOps.end(), "ADD"), dataOps.end());
  TemporaryTarget missingOperation(targetJson);
  const auto noAddTarget = cgra::TargetModel::loadFromFile(missingOperation.path());
  const auto failed = cgra::target::TargetLegalizer::legalize(fixture, noAddTarget);
  expect(!failed.ok() && !failed.dfg, "removing ADD rejects Generic Add");
}

} // namespace

int main() {
  try {
    const auto target = cgra::TargetModel::loadFromFile(TargetPath);
    testTargetOperationQueries(target);
    const auto fixture = cgra::ir::fixtures::simpleAdd();
    testPositiveFixtures(target);
    testUnsupportedGenericOperations(target);
    testTargetMutation(fixture);
    std::cout << "CGRA_TARGET_LEGALIZER_TEST_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_TARGET_LEGALIZER_TEST_FAIL: " << error.what() << '\n';
    return 1;
  }
}
