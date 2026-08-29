// SPDX-License-Identifier: MIT
#include "cgra/ABI/CompileKernel.h"
#include "cgra/ABI/KernelInvocation.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void usage(const char* name) {
  std::cerr << "usage: " << name
            << " input.generic_dfg.json --target target.json --invocation invocation.json"
               " --artifact-dir dir -o manifest.json [--abi-layout-out path]"
               " [--mode hardware|mapping-research]"
               " [--mapping-objective optimize-ii|find-any-feasible]"
               " [--enable-feasibility-fallback] [--low-ii-window N] [--max-safe-ii N]"
               " [--enable-recurrence-ingress]"
               " [--enable-software-rotation] [--max-rotation-factor N]"
               " [--enable-rf-port-aware|--disable-rf-port-aware]"
               " [--max-control-period N]"
               " [--max-ii N] [--min-ii N] [--max-node-candidates N]"
               " [--max-backtracks N] [--max-route-calls N] [--max-route-states N]"
               " [--no-virtual-hold]\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 8) {
    usage(argv[0]);
    return 2;
  }
  try {
    const auto inputPath = std::filesystem::path(argv[1]);
    std::filesystem::path targetPath;
    std::filesystem::path invocationPath;
    std::filesystem::path artifactDirectory;
    std::filesystem::path outputPath;
    std::filesystem::path layoutPath;
    std::string kernelName;
    cgra::abi::CompileKernelOptions options;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc)
          throw std::invalid_argument("missing value for " + arg);
        return argv[++i];
      };
      if (arg == "--target")
        targetPath = next();
      else if (arg == "--invocation")
        invocationPath = next();
      else if (arg == "--artifact-dir")
        artifactDirectory = next();
      else if (arg == "-o" || arg == "--output")
        outputPath = next();
      else if (arg == "--abi-layout-out")
        layoutPath = next();
      else if (arg == "--kernel-name")
        kernelName = next();
      else if (arg == "--mode") {
        const auto mode = next();
        if (mode == "hardware")
          options.backend.mode = cgra::pipeline::CompileDFGMode::HardwareExecutable;
        else if (mode == "mapping-research")
          options.backend.mode = cgra::pipeline::CompileDFGMode::MappingResearch;
        else
          throw std::invalid_argument("unsupported compile mode: " + mode);
      } else if (arg == "--pipeline-stop") {
        const auto stop = next();
        if (stop != "modulo-map")
          throw std::invalid_argument("unsupported pipeline stop: " + stop);
        options.backend.mode = cgra::pipeline::CompileDFGMode::MappingResearch;
      } else if (arg == "--mapping-objective") {
        const auto objective = next();
        if (objective == "optimize-ii")
          options.backend.mapper.objective = cgra::mapping::MappingObjective::OptimizeII;
        else if (objective == "find-any-feasible")
          options.backend.mapper.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
        else
          throw std::invalid_argument("unsupported mapping objective: " + objective);
      } else if (arg == "--enable-feasibility-fallback") {
        options.backend.mapper.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
        options.backend.mapper.feasibilityFallback.enabled = true;
      } else if (arg == "--low-ii-window") {
        options.backend.mapper.feasibilityFallback.lowIIWindow = std::stoul(next());
      } else if (arg == "--max-safe-ii") {
        options.backend.mapper.feasibilityFallback.maxSafeII = std::stoul(next());
      } else if (arg == "--enable-recurrence-ingress") {
        options.backend.normalizeRecurrenceIngress = true;
      } else if (arg == "--enable-software-rotation") {
        options.backend.rfAllocation.enableSoftwareRotation = true;
      } else if (arg == "--enable-rf-port-aware") {
        options.backend.mapper.rfPortAware.enabled = true;
      } else if (arg == "--disable-rf-port-aware") {
        options.backend.mapper.rfPortAware.enabled = false;
      } else if (arg == "--max-rotation-factor") {
        options.backend.rfAllocation.maxRotationFactor = std::stoul(next());
      } else if (arg == "--max-control-period") {
        options.backend.rfAllocation.maxControlPeriodCycles = std::stoul(next());
      }
      else if (arg == "--max-ii")
        options.backend.mapper.maxII = std::stoul(next());
      else if (arg == "--min-ii")
        options.backend.mapper.minII = std::stoul(next());
      else if (arg == "--max-node-candidates")
        options.backend.mapper.budget.maxNodeCandidateAttempts = std::stoull(next());
      else if (arg == "--max-backtracks")
        options.backend.mapper.budget.maxBacktracks = std::stoull(next());
      else if (arg == "--max-route-calls")
        options.backend.mapper.budget.maxRouteSearchCalls = std::stoull(next());
      else if (arg == "--max-route-states")
        options.backend.mapper.budget.perRouteBudget.maxStateExpansions = std::stoull(next());
      else if (arg == "--no-virtual-hold")
        options.backend.mapper.routeOptions.allowVirtualHold = false;
      else {
        usage(argv[0]);
        return 2;
      }
    }
    if (targetPath.empty() || invocationPath.empty() || artifactDirectory.empty() ||
        outputPath.empty())
      throw std::invalid_argument("target, invocation, artifact directory and output are required");
    targetPath = std::filesystem::absolute(targetPath);
    const auto target = cgra::TargetModel::loadFromFile(targetPath);
    const auto dfg = cgra::ir::readJson(inputPath);
    const auto signature = cgra::abi::inferSignature(dfg);
    std::ifstream invocationFile(invocationPath);
    if (!invocationFile)
      throw std::runtime_error("cannot open invocation JSON");
    std::stringstream invocationText;
    invocationText << invocationFile.rdbuf();
    options.invocation = cgra::abi::parseInvocation(invocationText.str(), signature);
    options.abi.kernelName = kernelName;
    options.backend.targetPath = targetPath;
    options.backend.artifactDirectory = artifactDirectory;
    options.backend.programName = kernelName.empty() ? dfg.name() : kernelName;
    if (options.backend.mapper.maxII == 0)
      options.backend.mapper.maxII = 8;
    if (options.backend.mapper.budget.maxNodeCandidateAttempts == 0)
      options.backend.mapper.budget.maxNodeCandidateAttempts = 100000;
    if (options.backend.mapper.budget.maxBacktracks == 0)
      options.backend.mapper.budget.maxBacktracks = 50000;
    if (options.backend.mapper.budget.maxRouteSearchCalls == 0)
      options.backend.mapper.budget.maxRouteSearchCalls = 100000;
    if (options.backend.mapper.budget.perRouteBudget.maxStateExpansions == 0)
      options.backend.mapper.budget.perRouteBudget.maxStateExpansions = 10000;
    if (options.backend.mapper.budget.perRouteBudget.maxQueuePushes == 0)
      options.backend.mapper.budget.perRouteBudget.maxQueuePushes = 20000;
    options.backend.rfAllocation.budget.maxColoringDecisions = 100000;
    options.backend.rfAllocation.budget.maxColoringBacktracks = 100000;
    options.backend.materializationBudget.maxExplicitBoundaryCycles = 1000000;
    options.backend.materializationBudget.maxExplicitBoundaryEvents = 1000000;
    const auto result = cgra::abi::compileKernel(dfg, target, options);
    if (!result.ok()) {
      std::cerr << cgra::abi::toString(result.status) << ": " << result.message << '\n';
      return 1;
    }
    if (!outputPath.parent_path().empty())
      std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream output(outputPath, std::ios::trunc);
    if (!output)
      throw std::runtime_error("cannot write generated manifest");
    if (options.backend.mode == cgra::pipeline::CompileDFGMode::MappingResearch)
      output << cgra::mapping::toJson(*result.backend->moduloMapping) << '\n';
    else
      output << result.backend->manifest->json << '\n';
    if (!output)
      throw std::runtime_error("cannot flush generated compiler output");
    const auto actualLayoutPath =
        layoutPath.empty() ? artifactDirectory / "kernel_abi_layout.json" : layoutPath;
    if (!actualLayoutPath.parent_path().empty())
      std::filesystem::create_directories(actualLayoutPath.parent_path());
    std::ofstream layout(actualLayoutPath, std::ios::trunc);
    if (!layout)
      throw std::runtime_error("cannot write kernel ABI layout");
    layout << cgra::abi::toJson(*result.abiLayout, &*result.signature, &options.invocation);
    if (!layout)
      throw std::runtime_error("cannot flush kernel ABI layout");
    std::cout << "status: success\nmode: " << cgra::pipeline::toString(options.backend.mode)
              << "\ntrip count: " << options.invocation.tripCount
              << "\nmapped ii: " << result.backend->stats.mappedII
              << "\nhardware executable: "
              << (result.backend->hardwareExecutable() ? "true" : "false")
              << "\noutput: " << outputPath << "\nabi layout: " << actualLayoutPath << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgrac-compile-kernel: " << error.what() << '\n';
    return 2;
  }
}
