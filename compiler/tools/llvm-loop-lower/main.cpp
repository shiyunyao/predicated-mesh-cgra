// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* name) {
  std::cerr << "usage: " << name
            << " input.ll|input.bc --function name [--loop-header block]"
               " --artifact-dir dir -o generic_dfg.json\n"
               "       "
            << name << " input.ll|input.bc [--function name] --list-loops\n";
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
    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    bool listLoops = false;
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
      else if (argument == "-o" || argument == "--output")
        outputPath = next();
      else if (argument == "--list-loops")
        listLoops = true;
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
      auto printFunction = [](llvm::Function& function) {
        llvm::DominatorTree dominatorTree(function);
        llvm::LoopInfo loopInfo(dominatorTree);
        std::function<void(llvm::Loop*)> visit = [&](llvm::Loop* loop) {
          if (loop->isInnermost()) {
            std::cout << function.getName().str()
                      << " header=" << blockName(function, *loop->getHeader())
                      << " blocks=" << loop->getBlocks().size() << " depth=" << loop->getLoopDepth()
                      << '\n';
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
      writeArtifact(artifactDirectory / "01_loop_selection.json",
                    result.metadata ? result.toJson() : "{}\n");
      writeArtifact(artifactDirectory / "02_recurrence_analysis.json", result.toJson());
      writeArtifact(artifactDirectory / "02_if_conversion.json", result.toJson());
      writeArtifact(artifactDirectory / "02_loop_control_slice.json", result.toJson());
      writeArtifact(artifactDirectory / "03_frontend_provenance.json", result.toJson());
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
    writeArtifact(outputPath, cgra::ir::toJson(*result.dfg));
    std::cout << "status: success\nfunction: " << result.metadata->functionName
              << "\nloop: " << result.metadata->loopHeader << "\noutput: " << outputPath << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgra-llvm-loop-lower: " << error.what() << '\n';
    return 2;
  }
}
