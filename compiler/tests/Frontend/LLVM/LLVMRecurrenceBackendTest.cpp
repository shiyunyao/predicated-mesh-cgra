// SPDX-License-Identifier: MIT
#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetLegalizer.h"
#include "cgra/Target/TargetModel.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef CGRA_REPOSITORY_ROOT
#define CGRA_REPOSITORY_ROOT "."
#endif

namespace {

const char* kReduction = R"IR(
define i32 @reduction(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %next, %loop ]
  %iv.next = add i32 %iv, 1
  %next = add i32 %sum, %x
  %cmp = icmp ult i32 %iv.next, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
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
    auto module = llvm::parseAssemblyString(kReduction, diagnostic, context);
    expect(module != nullptr, "reduction fixture must parse");

    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    options.functionName = "reduction";
    const auto frontend = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    expect(frontend.ok(), "LLVM recurrence must lower before backend preservation");
    expect(frontend.dfg->edges().size() == 1 && frontend.dfg->edge(0).distance == 1,
           "frontend recurrence distance must be one");

    const auto target = cgra::TargetModel::loadFromFile(
        std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v3.json");
    const auto legal = cgra::target::TargetLegalizer::legalize(*frontend.dfg, target);
    expect(legal.ok(), "recurrence must survive target legalization");
    const auto mii = cgra::analysis::MIIAnalyzer::analyze(*legal.dfg, target);
    expect(mii.ok() && mii.recurrenceMII >= 1, "MII analyzer must observe the LLVM recurrence");
    cgra::mapping::ModuloMapperOptions mapper;
    mapper.maxII = std::max<std::uint32_t>(mii.mii, 1U);
    mapper.budget.maxNodeCandidateAttempts = 10000;
    mapper.budget.maxBacktracks = 10000;
    mapper.budget.maxRouteSearchCalls = 10000;
    const auto mapped = cgra::mapping::ModuloMapper::map(*legal.dfg, target, mapper);
    expect(mapped.ok() && mapped.mapping, "legalized LLVM recurrence must map");
    expect(cgra::mapping::ModuloMappingVerifier::verify(*legal.dfg, target, *mapped.mapping).ok(),
           "mapped LLVM recurrence must pass the mapping verifier");
    std::cout << "CGRA_LLVM_RECURRENCE_BACKEND_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_LLVM_RECURRENCE_BACKEND_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
