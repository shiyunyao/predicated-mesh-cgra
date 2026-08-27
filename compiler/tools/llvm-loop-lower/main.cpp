// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelSignature.h"
#include "cgra/Frontend/LLVM/FrontendInvocationValidation.h"
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void usage(const char* name) {
  std::cerr << "usage: " << name
            << " input.ll|input.bc --function name [--loop-header block]"
               " --artifact-dir dir -o generic_dfg.json\n"
               "       [--invocation invocation.json]\n"
            << name << " input.ll|input.bc [--function name] --list-loops [--json]\n";
}

std::string blockName(const llvm::Function& function, const llvm::BasicBlock& block) {
  if (block.hasName())
    return block.getName().str();
  std::uint32_t ordinal = 0;
  for (const auto& candidate : function) {
    if (&candidate == &block)
      break;
    ++ordinal;
  }
  return "bb." + std::to_string(ordinal);
}

std::optional<std::uint64_t> staticTripCount(llvm::Function& function, llvm::Loop& loop,
                                             llvm::DominatorTree& dominatorTree,
                                             llvm::LoopInfo& loopInfo) {
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(function);
  llvm::ScalarEvolution scalarEvolution(function, libraryInfo, assumptions, dominatorTree,
                                        loopInfo);
  const auto count = scalarEvolution.getSmallConstantTripCount(&loop);
  if (count == 0)
    return std::nullopt;
  return count;
}

nlohmann::json loopFeatures(const llvm::Loop& loop) {
  nlohmann::json result = {{"opcode_histogram", nlohmann::json::object()},
                           {"type_histogram", nlohmann::json::object()},
                           {"icmp_predicate_histogram", nlohmann::json::object()},
                           {"counts",
                            {{"phis", 0},
                             {"branches", 0},
                             {"internal_conditional_branches", 0},
                             {"loads", 0},
                             {"stores", 0},
                             {"geps", 0},
                             {"calls_or_intrinsics", 0},
                             {"atomics_or_fences", 0},
                             {"volatile", 0},
                             {"pointer_phis", 0},
                             {"selects", 0}}}};
  const auto increment = [&](nlohmann::json& value) {
    value = value.is_number_unsigned() ? value.get<std::uint64_t>() + 1 : 1;
  };
  const auto countType = [&](const llvm::Type* type) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    type->print(stream);
    auto& count = result["type_histogram"][stream.str()];
    count = count.is_number_unsigned() ? count.get<std::uint64_t>() + 1 : 1;
  };
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      auto& opcodeCount = result["opcode_histogram"][instruction.getOpcodeName()];
      opcodeCount = opcodeCount.is_number_unsigned() ? opcodeCount.get<std::uint64_t>() + 1 : 1;
      countType(instruction.getType());
      for (const auto& operand : instruction.operands())
        countType(operand->getType());
      if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
        increment(result["counts"]["phis"]);
        if (phi->getType()->isPointerTy())
          increment(result["counts"]["pointer_phis"]);
      }
      if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
        increment(result["counts"]["branches"]);
        if (branch->isConditional() && loop.contains(branch->getSuccessor(0)) &&
            loop.contains(branch->getSuccessor(1)))
          increment(result["counts"]["internal_conditional_branches"]);
      }
      if (llvm::isa<llvm::LoadInst>(instruction))
        increment(result["counts"]["loads"]);
      if (llvm::isa<llvm::StoreInst>(instruction))
        increment(result["counts"]["stores"]);
      if (llvm::isa<llvm::GetElementPtrInst>(instruction))
        increment(result["counts"]["geps"]);
      if (llvm::isa<llvm::CallBase>(instruction))
        increment(result["counts"]["calls_or_intrinsics"]);
      if (llvm::isa<llvm::AtomicRMWInst>(instruction) ||
          llvm::isa<llvm::AtomicCmpXchgInst>(instruction) ||
          llvm::isa<llvm::FenceInst>(instruction))
        increment(result["counts"]["atomics_or_fences"]);
      if ((llvm::isa<llvm::LoadInst>(instruction) &&
           llvm::cast<llvm::LoadInst>(instruction).isVolatile()) ||
          (llvm::isa<llvm::StoreInst>(instruction) &&
           llvm::cast<llvm::StoreInst>(instruction).isVolatile()))
        increment(result["counts"]["volatile"]);
      if (llvm::isa<llvm::SelectInst>(instruction))
        increment(result["counts"]["selects"]);
      if (const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
        const auto predicate = llvm::CmpInst::getPredicateName(compare->getPredicate());
        auto& predicateCount = result["icmp_predicate_histogram"][predicate.str()];
        increment(predicateCount);
      }
    }
  }
  return result;
}

void writeArtifact(const std::filesystem::path& path, const std::string& text) {
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot write artifact: " + temporary);
  output << text;
  if (!text.empty() && text.back() != '\n')
    output << '\n';
  if (!output)
    throw std::runtime_error("cannot flush artifact: " + temporary);
  output.close();
  std::filesystem::rename(temporary, path);
}

std::string moduleText(const llvm::Module& module) {
  std::string text;
  llvm::raw_string_ostream output(text);
  module.print(output, nullptr);
  return output.str();
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  try {
    const auto inputPath = std::filesystem::path(argv[1]);
    std::filesystem::path artifactDirectory;
    std::filesystem::path outputPath;
    std::filesystem::path invocationPath;
    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    bool listLoops = false;
    bool listLoopsJson = false;
    for (int index = 2; index < argc; ++index) {
      const std::string argument = argv[index];
      auto next = [&]() -> std::string {
        if (index + 1 >= argc)
          throw std::invalid_argument("missing value for " + argument);
        return argv[++index];
      };
      if (argument == "--function")
        options.functionName = next();
      else if (argument == "--loop-header")
        options.loopHeader = next();
      else if (argument == "--artifact-dir")
        artifactDirectory = next();
      else if (argument == "--invocation")
        invocationPath = next();
      else if (argument == "-o" || argument == "--output")
        outputPath = next();
      else if (argument == "--list-loops")
        listLoops = true;
      else if (argument == "--json")
        listLoopsJson = true;
      else {
        usage(argv[0]);
        return 2;
      }
    }
    if (!listLoops && (options.functionName.empty() || outputPath.empty()))
      throw std::invalid_argument("--function and -o are required");

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseIRFile(inputPath.string(), diagnostic, context);
    if (!module) {
      diagnostic.print(argv[0], llvm::errs());
      return 1;
    }
    if (listLoops) {
      nlohmann::json loopsJson = {{"schema", "cgra.llvm_loop_inventory.v1"},
                                  {"loops", nlohmann::json::array()}};
      auto printFunction = [&](llvm::Function& function) {
        llvm::DominatorTree dominatorTree(function);
        llvm::LoopInfo loopInfo(dominatorTree);
        std::function<void(llvm::Loop*)> visit = [&](llvm::Loop* loop) {
          if (loop->isInnermost()) {
            llvm::SmallVector<llvm::BasicBlock*, 8> exits;
            llvm::SmallVector<llvm::BasicBlock*, 8> latches;
            loop->getExitBlocks(exits);
            loop->getLoopLatches(latches);
            const auto staticCount = staticTripCount(function, *loop, dominatorTree, loopInfo);
            nlohmann::json entry = {{"function", function.getName().str()},
                                    {"header", blockName(function, *loop->getHeader())},
                                    {"depth", loop->getLoopDepth()},
                                    {"block_count", loop->getBlocks().size()},
                                    {"latch_count", latches.size()},
                                    {"exit_count", exits.size()},
                                    {"features", loopFeatures(*loop)},
                                    {"static_trip_count", staticCount ? nlohmann::json(*staticCount)
                                                                      : nlohmann::json(nullptr)}};
            nlohmann::json shape = {{"kind", "unsupported"},
                                    {"unique_preheader", loop->getLoopPreheader() != nullptr},
                                    {"unique_latch", latches.size() == 1},
                                    {"unique_exit", exits.size() == 1},
                                    {"internal_conditional_branches",
                                     entry["features"]["counts"]["internal_conditional_branches"]}};
            if (loop->getBlocks().size() == 1)
              shape["kind"] = "single_block";
            else {
              const auto linear =
                  cgra::frontend::llvm_frontend::discoverLinearLoopRegion(function, *loop);
              if (linear.ok())
                shape["kind"] = "linear_multiblock";
            }
            entry["shape"] = std::move(shape);
            loopsJson["loops"].push_back(std::move(entry));
            if (!listLoopsJson)
              std::cout << function.getName().str()
                        << " header=" << blockName(function, *loop->getHeader())
                        << " blocks=" << loop->getBlocks().size()
                        << " depth=" << loop->getLoopDepth() << '\n';
            return;
          }
          for (auto* child : *loop)
            visit(child);
        };
        for (auto* loop : loopInfo)
          visit(loop);
      };
      if (options.functionName.empty()) {
        for (auto& function : *module)
          if (!function.isDeclaration())
            printFunction(function);
      } else if (auto* function = module->getFunction(options.functionName)) {
        printFunction(*function);
      } else {
        throw std::invalid_argument("function not found: " + options.functionName);
      }
      if (listLoopsJson)
        std::cout << loopsJson.dump(2) << '\n';
      return 0;
    }
    if (!artifactDirectory.empty()) {
      std::filesystem::create_directories(artifactDirectory);
      writeArtifact(artifactDirectory / "00_input.ll", moduleText(*module));
    }

    const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    if (!result.ok()) {
      if (!artifactDirectory.empty())
        writeArtifact(artifactDirectory / "06_frontend_result.json", result.toJson());
      std::cerr << cgra::frontend::llvm_frontend::toString(result.status) << ": " << result.message
                << '\n';
      for (const auto& item : result.diagnostics)
        std::cerr << cgra::frontend::llvm_frontend::toString(item.code) << ": " << item.message
                  << '\n';
      return 1;
    }
    const auto verification =
        cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result);
    if (!artifactDirectory.empty()) {
      const auto resultJson = nlohmann::json::parse(result.toJson());
      writeArtifact(artifactDirectory / "01_loop_selection.json",
                    result.metadata ? (nlohmann::json{{"schema", "cgra.llvm_loop_selection.v1"},
                                                      {"metadata", resultJson["metadata"]}}
                                           .dump(2) +
                                       "\n")
                                    : "{}\n");
      writeArtifact(artifactDirectory / "02_recurrence_analysis.json",
                    (nlohmann::json{{"schema", "cgra.llvm_recurrence_analysis.v1"},
                                    {"recurrences", resultJson["provenance"]["recurrences"]}}
                         .dump(2) +
                     "\n"));
      writeArtifact(artifactDirectory / "02_loop_control_slice.json",
                    (nlohmann::json{{"schema", "cgra.llvm_loop_control_slice.v1"},
                                    {"instructions", resultJson["provenance"]["control_slice"]}}
                         .dump(2) +
                     "\n"));
      writeArtifact(artifactDirectory / "03_frontend_provenance.json",
                    (nlohmann::json{{"schema", "cgra.llvm_frontend_provenance.v1"},
                                    {"nodes", resultJson["provenance"]["nodes"]},
                                    {"externals", resultJson["provenance"]["externals"]},
                                    {"live_outs", resultJson["provenance"]["live_outs"]}}
                         .dump(2) +
                     "\n"));
      if (resultJson["provenance"].contains("if_conversions"))
        writeArtifact(
            artifactDirectory / "02_if_conversion.json",
            (nlohmann::json{{"schema", "cgra.llvm_if_conversion.v1"},
                            {"if_conversions", resultJson["provenance"]["if_conversions"]}}
                 .dump(2) +
             "\n"));
      if (resultJson["provenance"].contains("memory_accesses"))
        writeArtifact(
            artifactDirectory / "02_memory_analysis.json",
            (nlohmann::json{{"schema", "cgra.llvm_memory_analysis.v1"},
                            {"accesses", resultJson["provenance"]["memory_accesses"]},
                            {"dependences", resultJson["provenance"]["memory_dependences"]}}
                 .dump(2) +
             "\n"));
      if (resultJson["provenance"].contains("linear_loop"))
        writeArtifact(artifactDirectory / "01_linear_loop.json",
                      resultJson["provenance"]["linear_loop"].dump(2) + "\n");
      writeArtifact(artifactDirectory / "04_generic_dfg.json", cgra::ir::toJson(*result.dfg));
      writeArtifact(artifactDirectory / "05_generic_dfg_verification.json",
                    cgra::ir::DFGVerifier::verify(*result.dfg).toJson());
      writeArtifact(artifactDirectory / "06_frontend_result.json", result.toJson());
      writeArtifact(artifactDirectory / "07_frontend_verification.json", verification.toJson());
    }
    if (!verification.ok()) {
      std::cerr << verification.format() << '\n';
      return 1;
    }
    if (!invocationPath.empty()) {
      std::ifstream input(invocationPath);
      if (!input)
        throw std::runtime_error("cannot read invocation: " + invocationPath.string());
      const std::string invocationJson((std::istreambuf_iterator<char>(input)),
                                       std::istreambuf_iterator<char>());
      const auto signature = cgra::abi::inferSignature(*result.dfg);
      const auto invocation = cgra::abi::parseInvocation(invocationJson, signature);
      const auto invocationReport =
          cgra::frontend::llvm_frontend::validateFrontendInvocation(*result.metadata, invocation);
      if (!artifactDirectory.empty())
        writeArtifact(artifactDirectory / "08_invocation_validation.json",
                      invocationReport.toJson());
      if (!invocationReport.ok()) {
        std::cerr << invocationReport.message << '\n';
        return 1;
      }
    }
    writeArtifact(outputPath, cgra::ir::toJson(*result.dfg));
    std::cout << "status: success\nfunction: " << result.metadata->functionName
              << "\nloop: " << result.metadata->loopHeader << "\noutput: " << outputPath << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgra-llvm-loop-lower: " << error.what() << '\n';
    return 2;
  }
}
