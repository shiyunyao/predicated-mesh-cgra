// SPDX-License-Identifier: MIT
#include "cgra/ABI/CompileKernel.h"
#include "cgra/ABI/KernelInvocation.h"
#include "cgra/IR/DFGSerialization.h"
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
               " [--max-ii N] [--min-ii N] [--no-virtual-hold]\n";
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
      else if (arg == "--max-ii")
        options.backend.mapper.maxII = std::stoul(next());
      else if (arg == "--min-ii")
        options.backend.mapper.minII = std::stoul(next());
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
    options.backend.mapper.budget.maxNodeCandidateAttempts = 100000;
    options.backend.mapper.budget.maxBacktracks = 50000;
    options.backend.mapper.budget.maxRouteSearchCalls = 100000;
    options.backend.mapper.budget.perRouteBudget.maxStateExpansions = 10000;
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
    std::ofstream manifest(outputPath, std::ios::trunc);
    if (!manifest)
      throw std::runtime_error("cannot write generated manifest");
    manifest << result.backend->manifest->json << '\n';
    if (!manifest)
      throw std::runtime_error("cannot flush generated manifest");
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
    std::cout << "status: success\ntrip count: " << options.invocation.tripCount
              << "\nmanifest: " << outputPath << "\nabi layout: " << actualLayoutPath << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgrac-compile-kernel: " << error.what() << '\n';
    return 2;
  }
}
