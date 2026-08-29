// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/PredicateSSA.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <iostream>
#include <stdexcept>

namespace {

const char* kSequentialDiamonds = R"IR(
define void @sequential(i1 %p, i1 %q) {
entry:
  br label %header
header:
  br i1 %p, label %true1, label %false1
true1:
  br label %merge1
false1:
  br label %merge1
merge1:
  br i1 %q, label %true2, label %false2
true2:
  br label %merge2
false2:
  br label %merge2
merge2:
  br i1 %q, label %header, label %exit
exit:
  ret void
}
)IR";

const char* kSequentialDiamondsReordered = R"IR(
define void @sequential(i1 %p, i1 %q) {
entry:
  br label %header
false2:
  br label %merge2
true1:
  br label %merge1
merge2:
  br i1 %q, label %header, label %exit
header:
  br i1 %p, label %true1, label %false1
true2:
  br label %merge2
false1:
  br label %merge1
merge1:
  br i1 %q, label %true2, label %false2
exit:
  ret void
}
)IR";

const char* kDynamicExit = R"IR(
define void @dynamic_exit(i1 %p, i1 %q) {
entry:
  br label %header
header:
  br i1 %p, label %body, label %exit
body:
  br i1 %q, label %early, label %latch
early:
  br label %exit2
latch:
  br label %header
exit:
  ret void
exit2:
  ret void
}
)IR";

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

} // namespace

int main() {
  try {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(kSequentialDiamonds, diagnostic, context);
    expect(module != nullptr, "predicate fixture must parse");
    auto& function = *module->getFunction("sequential");
    llvm::DominatorTree dominators(function);
    llvm::LoopInfo loops(dominators);
    auto headerIt = function.begin();
    ++headerIt;
    auto* header = &*headerIt;
    auto* loop = loops.getLoopFor(header);
    expect(loop != nullptr, "predicate fixture must contain a loop");

    const auto result = cgra::frontend::llvm_frontend::buildPredicateSSA(*loop);
    expect(result.ok(), "reducible sequential diamonds must build Predicate-SSA");
    expect(result.blockPredicates.size() == 7, "all loop blocks must receive predicates");
    expect(result.forBlock(header) != nullptr, "header predicate must be discoverable");
    for (const auto& block : result.blockPredicates)
      expect(block.expression != nullptr, "every block must have a predicate expression");

    llvm::LLVMContext reorderedContext;
    auto reorderedModule =
        llvm::parseAssemblyString(kSequentialDiamondsReordered, diagnostic, reorderedContext);
    expect(reorderedModule != nullptr, "reordered predicate fixture must parse");
    auto& reorderedFunction = *reorderedModule->getFunction("sequential");
    llvm::DominatorTree reorderedDominators(reorderedFunction);
    llvm::LoopInfo reorderedLoops(reorderedDominators);
    llvm::Loop* reorderedLoop = nullptr;
    for (auto* candidate : reorderedLoops)
      reorderedLoop = candidate;
    expect(reorderedLoop != nullptr, "reordered fixture must retain the same natural loop");
    const auto reorderedResult =
        cgra::frontend::llvm_frontend::buildPredicateSSA(*reorderedLoop);
    expect(reorderedResult.ok(), "textually reordered reducible CFG must build Predicate-SSA");
    expect(result.orderedBlocks.size() == reorderedResult.orderedBlocks.size(),
           "textual block reorder must retain the CFG block count");
    for (std::size_t index = 0; index < result.orderedBlocks.size(); ++index)
      expect(result.orderedBlocks[index]->getName() == reorderedResult.orderedBlocks[index]->getName(),
             "Predicate-SSA order must come from CFG edges, not textual layout");

    llvm::LLVMContext exitContext;
    auto exitModule = llvm::parseAssemblyString(kDynamicExit, diagnostic, exitContext);
    expect(exitModule != nullptr, "dynamic-exit fixture must parse");
    auto& exitFunction = *exitModule->getFunction("dynamic_exit");
    llvm::DominatorTree exitDominators(exitFunction);
    llvm::LoopInfo exitLoops(exitDominators);
    llvm::Loop* exitLoop = nullptr;
    for (auto* candidate : exitLoops)
      exitLoop = candidate;
    expect(exitLoop != nullptr, "dynamic-exit fixture must contain a loop");
    const auto exitResult = cgra::frontend::llvm_frontend::buildPredicateSSA(*exitLoop);
    expect(exitResult.status ==
               cgra::frontend::llvm_frontend::PredicateSSAStatus::DynamicExit,
           "multiple loop exits must remain an explicit dynamic-control rejection");
    std::cout << "PREDICATE_SSA_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PREDICATE_SSA_FAIL: " << error.what() << '\n';
    return 1;
  }
}
