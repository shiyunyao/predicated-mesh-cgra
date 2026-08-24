// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGSerialization.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
const std::filesystem::path Root = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::filesystem::path uniqueDirectory(std::string_view stem) {
  const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  auto path =
      std::filesystem::temp_directory_path() / (std::string(stem) + "-" + std::to_string(stamp));
  std::filesystem::create_directories(path);
  return path;
}

std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'')
      quoted += "'\\''";
    else
      quoted += character;
  }
  quoted += "'";
  return quoted;
}

cgra::pipeline::CompileDFGOptions options(const std::filesystem::path& artifacts) {
  cgra::pipeline::CompileDFGOptions result;
  result.tripCount = 4;
  result.targetPath = Root / "target/cgra_v3.json";
  result.artifactDirectory = artifacts;
  result.programName = "e2e_fixed_addr_load_add_store";
  result.scratchpadPreload = {{0, 7}, {1, 11}, {2, 0}};
  result.mapper.maxII = 8;
  result.mapper.budget.maxNodeCandidateAttempts = 100'000;
  result.mapper.budget.maxBacktracks = 50'000;
  result.mapper.budget.maxRouteSearchCalls = 100'000;
  result.mapper.budget.perRouteBudget.maxStateExpansions = 10'000;
  result.mapper.budget.perRouteBudget.maxQueuePushes = 20'000;
  result.rfAllocation.budget.maxColoringDecisions = 100'000;
  result.rfAllocation.budget.maxColoringBacktracks = 100'000;
  result.materializationBudget.maxExplicitBoundaryCycles = 1'000'000;
  result.materializationBudget.maxExplicitBoundaryEvents = 1'000'000;
  return result;
}

void verifyManifest(const std::filesystem::path& manifest) {
  const auto validator = Root / "tools/validate_program.py";
  const auto command =
      "python3 " + shellQuote(validator.string()) + " " + shellQuote(manifest.string());
  expect(std::system(command.c_str()) == 0,
         "retained program validator rejected generated manifest");
}

void checkArtifacts(const std::filesystem::path& artifacts) {
  const std::vector<std::string> names = {
      "00_input.generic_dfg.json",
      "01_generic_dfg_verification.json",
      "02_legalization.json",
      "03_target_dfg.json",
      "04_target_dfg_verification.json",
      "05_mii.json",
      "06_mapper_report.json",
      "07_modulo_mapping.json",
      "08_modulo_mapping_verification.json",
      "09_stage_report.json",
      "10_staged_mapping.json",
      "11_stage_verification.json",
      "12_rf_report.json",
      "13_rf_allocated_mapping.json",
      "14_rf_verification.json",
      "15_materialization_report.json",
      "16_materialized_schedule.json",
      "17_materialization_verification.json",
      "18_target_controls.json",
      "19_target_control_verification.json",
      "20_program_manifest.json",
      "21_lowering_report.json",
      "compiler_pipeline_report.json",
  };
  for (const auto& name : names)
    expect(std::filesystem::is_regular_file(artifacts / name), "missing compiler artifact " + name);
}

void checkSemanticManifest(const std::string& manifestText) {
  const auto manifest = Json::parse(manifestText);
  expect(manifest.at("schema") == "cgra.program_manifest.v1", "wrong manifest schema");
  expect(manifest.at("loop").at("trip_count") > 0, "manifest kernel repeat count is empty");
  expect(manifest.at("loop").at("ii") >= 1, "manifest II is invalid");
  expect(manifest.at("program").at("tiles").size() == 16, "manifest is not a 4x4 tile image");

  std::size_t preloadEntries = 0;
  for (const auto& tile : manifest.at("program").at("tiles"))
    preloadEntries += tile.at("scratchpad_preload").size();
  expect(preloadEntries == 3, "shared scratchpad preload was duplicated or dropped");
}

void checkSourceAndInputPoison(const std::filesystem::path& fixture,
                               const cgra::TargetModel& target,
                               const cgra::pipeline::CompileDFGOptions& baselineOptions,
                               const std::string& baselineManifest) {
  const auto runGoldenObservation = [&](const std::string& manifestText,
                                        const std::filesystem::path& artifactDirectory,
                                        std::uint32_t expectedData) {
    const auto manifestPath = artifactDirectory / "poison_manifest.json";
    const auto replayDirectory = artifactDirectory / "replay";
    const auto expectationPath = artifactDirectory / "poison_expectation.json";
    std::filesystem::create_directories(replayDirectory);
    std::ofstream manifestOutput(manifestPath);
    manifestOutput << manifestText << '\n';
    manifestOutput.close();
    std::ofstream expectationOutput(expectationPath);
    expectationOutput << Json{{"trip_count", 4},
                              {"committed_stores",
                               Json::array(
                                   {Json{{"address", 2}, {"data", expectedData}, {"count", 4}}})},
                              {"unexpected_store_policy", "forbid"}}
                             .dump(2)
                      << '\n';
    expectationOutput.close();
    const auto prepare = "python3 " + shellQuote((Root / "tools/program_runner.py").string()) +
                         " --prepare " + shellQuote(manifestPath.string()) + " --out-dir " +
                         shellQuote(replayDirectory.string());
    expect(std::system(prepare.c_str()) == 0, "poison manifest golden preparation failed");
    const auto check = "python3 " +
                       shellQuote((Root / "tools/check_compiler_e2e_observations.py").string()) +
                       " --expectation " + shellQuote(expectationPath.string()) + " --golden " +
                       shellQuote((replayDirectory / "golden_trace.csv").string()) + " --rtl " +
                       shellQuote((replayDirectory / "golden_trace.csv").string());
    expect(std::system(check.c_str()) == 0, "poison manifest semantic observation failed");
  };

  std::ifstream sourceFile(fixture / "generic_dfg.json", std::ios::binary);
  const auto source = Json::parse(sourceFile);
  auto changedSource = source;
  for (auto& node : changedSource.at("nodes"))
    if (node.at("opcode") == "Add")
      node["opcode"] = "Sub";
  const auto changedDfg = cgra::ir::parse(changedSource.dump());
  auto changedOptions = baselineOptions;
  changedOptions.artifactDirectory = uniqueDirectory("cgra-compiler-e2e-sub");
  const auto changed = cgra::pipeline::compileGenericDFG(changedDfg, target, changedOptions);
  expect(changed.ok(), "SUB poison compilation failed: " + changed.message);
  expect(changed.manifest->json != baselineManifest,
         "opcode poison did not change the generated manifest");
  runGoldenObservation(changed.manifest->json, changedOptions.artifactDirectory, 0xFFFF'FFFCU);

  auto changedInputOptions = baselineOptions;
  changedInputOptions.artifactDirectory = uniqueDirectory("cgra-compiler-e2e-input");
  changedInputOptions.scratchpadPreload = {{0, 9}, {1, 5}, {2, 0}};
  const auto changedInput = cgra::pipeline::compileGenericDFG(
      cgra::ir::readJson(fixture / "generic_dfg.json"), target, changedInputOptions);
  expect(changedInput.ok(), "input poison compilation failed: " + changedInput.message);
  expect(changedInput.manifest->json != baselineManifest,
         "scratchpad input poison did not change the generated manifest");
  runGoldenObservation(changedInput.manifest->json, changedInputOptions.artifactDirectory, 14);
}

} // namespace

int main() {
  try {
    const auto fixture = Root / "compiler/tests/e2e/fixtures/fixed_addr_load_add_store";
    const auto generic = cgra::ir::readJson(fixture / "generic_dfg.json");
    const auto target = cgra::TargetModel::loadFromFile(Root / "target/cgra_v3.json");

    const auto firstArtifacts = uniqueDirectory("cgra-compiler-e2e-first");
    auto firstOptions = options(firstArtifacts);
    const auto first = cgra::pipeline::compileGenericDFG(generic, target, firstOptions);
    expect(first.ok(), "compiler pipeline failed: " + first.message);
    expect(first.manifest.has_value(), "successful pipeline returned no manifest");
    checkArtifacts(firstArtifacts);
    checkSemanticManifest(first.manifest->json);

    const auto firstManifest = firstArtifacts / "manifest.json";
    {
      std::ofstream output(firstManifest);
      output << first.manifest->json << '\n';
    }
    verifyManifest(firstManifest);
    checkSourceAndInputPoison(fixture, target, firstOptions, first.manifest->json);

    const auto secondArtifacts = uniqueDirectory("cgra-compiler-e2e-second");
    auto secondOptions = options(secondArtifacts);
    const auto second = cgra::pipeline::compileGenericDFG(generic, target, secondOptions);
    expect(second.ok(), "second deterministic pipeline run failed: " + second.message);
    expect(second.manifest->json == first.manifest->json,
           "identical compiler inputs did not produce byte-identical manifests");
    expect(std::filesystem::file_size(firstArtifacts / "07_modulo_mapping.json") ==
               std::filesystem::file_size(secondArtifacts / "07_modulo_mapping.json"),
           "deterministic mapping artifacts differ in size");

    std::cout << "compiler pipeline E2E smoke passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "compiler pipeline E2E smoke failed: " << error.what() << '\n';
    return 1;
  }
}
