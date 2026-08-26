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

const char* kRawDistanceOne = R"IR(
target datalayout = "e-p:64:64"
define void @memory_recurrence(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 1, %entry ], [ %inc, %loop ]
  %previous = sub i32 %i, 1
  %read.addr = getelementptr i32, i32* %A, i32 %previous
  %write.addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %read.addr, align 4
  %next = add i32 %value, 1
  store i32 %next, i32* %write.addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 5
  br i1 %done, label %loop, label %exit
exit:
  ret void
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
    auto module = llvm::parseAssemblyString(kRawDistanceOne, diagnostic, context);
    expect(module != nullptr, "memory recurrence fixture must parse");

    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    options.functionName = "memory_recurrence";
    const auto frontend = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    expect(frontend.ok(), "LLVM memory recurrence must lower before backend preservation");

    const auto genericMemory = std::ranges::find_if(frontend.dfg->edges(), [](const auto& edge) {
      const auto* memory = std::get_if<cgra::ir::MemoryEdgeInfo>(&edge.info);
      return memory && memory->dependence == cgra::ir::MemoryDepKind::RAW && edge.distance == 1;
    });
    expect(genericMemory != frontend.dfg->edges().end(),
           "frontend must emit the exact RAW distance-one recurrence");

    const auto target = cgra::TargetModel::loadFromFile(
        std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v3.json");
    const auto legal = cgra::target::TargetLegalizer::legalize(*frontend.dfg, target);
    expect(legal.ok(), "memory recurrence must survive target legalization");

    const auto targetMemory = std::ranges::find_if(legal.dfg->edges(), [](const auto& edge) {
      const auto* memory = std::get_if<cgra::ir::MemoryEdgeInfo>(&edge.info);
      return memory && memory->dependence == cgra::ir::MemoryDepKind::RAW && edge.distance == 1;
    });
    expect(targetMemory != legal.dfg->edges().end(),
           "Target DFG must retain RAW kind and distance");

    const auto mii = cgra::analysis::MIIAnalyzer::analyze(*legal.dfg, target);
    expect(mii.ok() && mii.recurrenceMII >= 1 && mii.recurrenceWitness,
           "MII analyzer must observe the memory recurrence cycle");
    expect(std::ranges::find(mii.recurrenceWitness->edges, targetMemory->id) !=
               mii.recurrenceWitness->edges.end(),
           "RecMII witness must include the RAW MemoryEdge");

    cgra::mapping::ModuloMapperOptions mapper;
    mapper.maxII = std::max<std::uint32_t>(mii.mii + 8, 12U);
    mapper.budget.maxNodeCandidateAttempts = 500000;
    mapper.budget.maxBacktracks = 500000;
    mapper.budget.maxRouteSearchCalls = 500000;
    const auto mapped = cgra::mapping::ModuloMapper::map(*legal.dfg, target, mapper);
    expect(mapped.ok() && mapped.mapping, "legalized memory recurrence must map");
    expect(cgra::mapping::ModuloMappingVerifier::verify(*legal.dfg, target, *mapped.mapping).ok(),
           "mapped memory recurrence must pass ModuloMappingVerifier");

    const auto& mappedMemory = mapped.mapping->dependence(targetMemory->id);
    expect(mappedMemory.kind == cgra::ir::Edge::Kind::Memory,
           "mapped dependence must remain a MemoryEdge");
    expect(!mappedMemory.transport,
           "MemoryEdge is ordering-only and must not consume mesh transport");

    std::cout << "CGRA_LLVM_MEMORY_BACKEND_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_LLVM_MEMORY_BACKEND_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
