// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"
#include "cgra/Pipeline/CompileDFG.h"
#include "cgra/Target/TargetModel.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef CGRA_REPOSITORY_ROOT
#define CGRA_REPOSITORY_ROOT "."
#endif

namespace {

const char* kSequentialBranches = R"IR(
define i32 @predicate_backend(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %p = icmp ult i32 %x, %y
  br i1 %p, label %then1, label %else1
then1:
  %q = icmp eq i32 %x, %y
  br i1 %q, label %then2, label %else2
else1:
  br label %merge2
then2:
  br label %merge2
else2:
  br label %merge2
merge2:
  br i1 %p, label %loop, label %exit
exit:
  ret i32 %x
}
)IR";

void expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

} // namespace

int main() {
  try {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(kSequentialBranches, diagnostic, context);
    expect(module != nullptr, "Predicate-SSA backend fixture must parse");

    cgra::frontend::llvm_frontend::LLVMFrontendOptions frontendOptions;
    frontendOptions.functionName = "predicate_backend";
    const auto frontend =
        cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, frontendOptions);
    expect(frontend.ok(), "multiple branches must lower before backend validation: " +
                              frontend.message);
    const auto frontendVerification = cgra::frontend::llvm_frontend::verifyFrontendResult(
        *module, frontendOptions, frontend);
    if (!frontendVerification.ok())
      std::cerr << frontendVerification.format() << '\n';
    expect(frontendVerification.ok(), "Predicate-SSA result must pass independent frontend verification");

    const auto target = cgra::TargetModel::loadFromFile(
        std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_mapping32_v1.json");
    cgra::pipeline::CompileDFGOptions options;
    options.mode = cgra::pipeline::CompileDFGMode::MappingResearch;
    options.mapper.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
    options.mapper.maxII = 8;
    options.mapper.budget.maxNodeCandidateAttempts = 5000;
    options.mapper.budget.maxBacktracks = 5000;
    options.mapper.budget.maxRouteSearchCalls = 5000;
    options.mapper.feasibilityFallback.enabled = true;
    options.mapper.feasibilityFallback.lowIIWindow = 2;
    options.mapper.feasibilityFallback.maxSafeII = 8;
    options.mapper.feasibilityFallback.maxLocalRepairs = 5000;
    options.mapper.feasibilityFallback.seed = 0;
    const auto compiled = cgra::pipeline::compileGenericDFG(*frontend.dfg, target, options);
    expect(compiled.ok(), "Predicate-SSA DFG must pass mapping, Stage, and finite RF checks: " +
                              compiled.message);
    expect(compiled.stats.safeII >= compiled.stats.mii &&
               compiled.stats.rfConstrainedMappings == 1 &&
               compiled.physicalRealizability.status ==
                   cgra::pipeline::PhysicalRealizabilityStatus::Feasible,
           "Predicate-SSA backend success must be a strict finite-RF mapping");

    std::cout << "PREDICATE_SSA_BACKEND_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PREDICATE_SSA_BACKEND_FAIL: " << error.what() << '\n';
    return 1;
  }
}
