// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGSerialization.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using Json = nlohmann::json;

void usage(const char* name) {
  std::cerr << "usage: " << name
            << " input.dfg.json --target target.json --trip-count N"
               " --artifact-dir dir -o manifest.json [--scratchpad-preload file]"
               " [--mapping-objective optimize-ii|find-any-feasible]"
               " [--enable-feasibility-fallback] [--low-ii-window N] [--max-safe-ii N]"
               " [--enable-recurrence-ingress]"
               " [--max-ii N] [--min-ii N] [--no-virtual-hold]\n";
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
} // namespace

int main(int argc, char** argv) {
  if (argc < 8) {
    usage(argv[0]);
    return 2;
  }
  try {
    const std::filesystem::path input = argv[1];
    cgra::pipeline::CompileDFGOptions options;
    std::filesystem::path output;
    std::filesystem::path preload;
    for (int index = 2; index < argc; ++index) {
      const std::string arg = argv[index];
      auto next = [&]() -> std::string {
        if (index + 1 >= argc)
          throw std::invalid_argument("missing value for " + arg);
        return argv[++index];
      };
      if (arg == "--target")
        options.targetPath = next();
      else if (arg == "--trip-count")
        options.tripCount = std::stoull(next());
      else if (arg == "--artifact-dir")
        options.artifactDirectory = next();
      else if (arg == "-o" || arg == "--output")
        output = next();
      else if (arg == "--scratchpad-preload")
        preload = next();
      else if (arg == "--max-ii")
        options.mapper.maxII = std::stoul(next());
      else if (arg == "--min-ii")
        options.mapper.minII = std::stoul(next());
      else if (arg == "--mapping-objective") {
        const auto objective = next();
        if (objective == "optimize-ii")
          options.mapper.objective = cgra::mapping::MappingObjective::OptimizeII;
        else if (objective == "find-any-feasible")
          options.mapper.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
        else
          throw std::invalid_argument("unsupported mapping objective: " + objective);
      } else if (arg == "--enable-feasibility-fallback") {
        options.mapper.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
        options.mapper.feasibilityFallback.enabled = true;
      } else if (arg == "--low-ii-window") {
        options.mapper.feasibilityFallback.lowIIWindow = std::stoul(next());
      } else if (arg == "--max-safe-ii") {
        options.mapper.feasibilityFallback.maxSafeII = std::stoul(next());
      } else if (arg == "--enable-recurrence-ingress") {
        options.normalizeRecurrenceIngress = true;
      }
      else if (arg == "--no-virtual-hold")
        options.mapper.routeOptions.allowVirtualHold = false;
      else if (arg == "--program-name")
        options.programName = next();
      else {
        usage(argv[0]);
        return 2;
      }
    }
    if (options.targetPath.empty() || options.artifactDirectory.empty() || output.empty() ||
        options.tripCount == 0)
      throw std::invalid_argument(
          "target, positive trip count, artifact directory and output are required");
    options.mapper.maxII = options.mapper.maxII == 0 ? 8 : options.mapper.maxII;
    options.mapper.budget.maxNodeCandidateAttempts = 100000;
    options.mapper.budget.maxBacktracks = 50000;
    options.mapper.budget.maxRouteSearchCalls = 100000;
    options.mapper.budget.perRouteBudget.maxStateExpansions = 10000;
    options.mapper.budget.perRouteBudget.maxQueuePushes = 20000;
    options.rfAllocation.budget.maxColoringDecisions = 100000;
    options.rfAllocation.budget.maxColoringBacktracks = 100000;
    options.materializationBudget.maxExplicitBoundaryCycles = 1000000;
    options.materializationBudget.maxExplicitBoundaryEvents = 1000000;
    if (!preload.empty()) {
      std::ifstream inputPreload(preload);
      if (!inputPreload)
        throw std::invalid_argument("cannot open scratchpad preload");
      Json values;
      inputPreload >> values;
      if (!values.is_object())
        throw std::invalid_argument(
            "scratchpad preload must be an object mapping addresses to values");
      for (const auto& [address, value] : values.items())
        options.scratchpadPreload.emplace_back(std::stoul(address), value.get<std::uint32_t>());
    }
    const auto target = cgra::TargetModel::loadFromFile(options.targetPath);
    const auto dfg = cgra::ir::readJson(input);
    auto result = cgra::pipeline::compileGenericDFG(dfg, target, options);
    if (!result.ok()) {
      std::cerr << cgra::pipeline::toString(result.status) << ": " << result.message << '\n';
      return 1;
    }
    std::ofstream manifest(output, std::ios::trunc);
    if (!manifest)
      throw std::runtime_error("cannot write generated manifest");
    manifest << result.manifest->json << '\n';
    const auto repositoryRoot = options.targetPath.parent_path().parent_path();
    const auto validator = repositoryRoot / "tools/validate_program.py";
    const auto validationCommand =
        "python3 " + shellQuote(validator.string()) + " " + shellQuote(output.string());
    if (std::system(validationCommand.c_str()) != 0) {
      result.status = cgra::pipeline::CompileDFGStatus::ManifestValidationFailure;
      result.message = "retained manifest validator rejected output";
      std::ofstream report(options.artifactDirectory / "compiler_pipeline_report.json",
                           std::ios::trunc);
      report << result.toJson() << '\n';
      std::cerr << "manifest_validation_failure: retained manifest validator rejected output\n";
      return 1;
    }
    if (!options.artifactDirectory.empty()) {
      std::ofstream report(options.artifactDirectory / "compiler_pipeline_report.json",
                           std::ios::trunc);
      report << result.toJson() << '\n';
    }
    std::cout << "status: success\n"
              << "mii: " << result.stats.mii << "\n"
              << "mapped ii: " << result.stats.mappedII << "\n"
              << "manifest: " << output << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgrac-compile-dfg: " << error.what() << '\n';
    return 2;
  }
}
