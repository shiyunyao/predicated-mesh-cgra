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
    std::cout << "PREDICATE_SSA_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PREDICATE_SSA_FAIL: " << error.what() << '\n';
    return 1;
  }
}
