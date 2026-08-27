// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMLinearLoop.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace cgra::frontend::llvm_frontend {
namespace {

LinearLoopAnalysisResult fail(LinearLoopStatus status, std::string message) {
  return LinearLoopAnalysisResult{status, std::move(message), std::nullopt};
}

bool isBackedge(const llvm::BasicBlock* source, const llvm::BasicBlock* destination,
                const llvm::Loop& loop) {
  return source == loop.getLoopLatch() && destination == loop.getHeader();
}

} // namespace

std::string_view toString(LinearLoopStatus status) noexcept {
  switch (status) {
  case LinearLoopStatus::Success:
    return "success";
  case LinearLoopStatus::NoPreheader:
    return "no_preheader";
  case LinearLoopStatus::NoLatch:
    return "no_latch";
  case LinearLoopStatus::ExitShape:
    return "exit_shape";
  case LinearLoopStatus::InternalConditionalBranch:
    return "internal_conditional_branch";
  case LinearLoopStatus::UnsupportedTerminator:
    return "unsupported_terminator";
  case LinearLoopStatus::NonLinearCFG:
    return "non_linear_cfg";
  case LinearLoopStatus::NonHeaderPHI:
    return "non_header_phi";
  }
  return "non_linear_cfg";
}

LinearLoopAnalysisResult discoverLinearLoopRegion(llvm::Loop& loop) {
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  auto* latch = loop.getLoopLatch();
  if (!preheader)
    return fail(LinearLoopStatus::NoPreheader, "canonical linear loop requires a unique preheader");
  if (!latch)
    return fail(LinearLoopStatus::NoLatch,
                "canonical linear loop requires a unique latch/backedge");

  llvm::SmallVector<llvm::BasicBlock*, 4> exitingBlocks;
  llvm::SmallVector<llvm::BasicBlock*, 4> exitBlocks;
  loop.getExitingBlocks(exitingBlocks);
  loop.getExitBlocks(exitBlocks);
  if (exitingBlocks.size() != 1 || exitBlocks.size() != 1)
    return fail(LinearLoopStatus::ExitShape,
                "canonical linear loop requires exactly one exiting block and one exit");
  auto* exiting = exitingBlocks.front();
  auto* exit = exitBlocks.front();
  if (exiting != header && exiting != latch)
    return fail(LinearLoopStatus::ExitShape,
                "linear-loop termination branch must be in the header or latch");
  auto* termination = llvm::dyn_cast<llvm::BranchInst>(exiting->getTerminator());
  if (!termination || !termination->isConditional() || termination->getNumSuccessors() != 2)
    return fail(LinearLoopStatus::ExitShape,
                "loop exit must be a two-way conditional termination branch");

  unsigned inLoopSuccessors = 0;
  unsigned exitSuccessors = 0;
  for (auto* successor : termination->successors()) {
    if (loop.contains(successor))
      ++inLoopSuccessors;
    else if (successor == exit)
      ++exitSuccessors;
    else
      return fail(LinearLoopStatus::ExitShape,
                  "termination branch has an unexpected outside successor");
  }
  if (inLoopSuccessors != 1 || exitSuccessors != 1)
    return fail(LinearLoopStatus::ExitShape,
                "termination branch must have one in-loop successor and one loop exit");

  for (auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      if (llvm::isa<llvm::PHINode>(instruction) && block != header)
        return fail(LinearLoopStatus::NonHeaderPHI,
                    "non-header PHI implies a hidden merge in a linear loop");
    }
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch) {
      return fail(LinearLoopStatus::UnsupportedTerminator,
                  "linear loop blocks must end in unconditional or termination branches");
    }
    if (branch == termination)
      continue;
    if (branch->isConditional()) {
      if (loop.contains(branch->getSuccessor(0)) && loop.contains(branch->getSuccessor(1)))
        return fail(LinearLoopStatus::InternalConditionalBranch,
                    "linear loop cannot contain an internal conditional branch");
      return fail(LinearLoopStatus::ExitShape,
                  "only the unique termination branch may be conditional");
    }
    if (!loop.contains(branch->getSuccessor(0)) && block != exiting)
      return fail(LinearLoopStatus::NonLinearCFG,
                  "non-termination unconditional branch leaves the loop");
  }

  // Reconstruct the path from CFG edges. Removing the canonical backedge makes a
  // linear loop an acyclic path, regardless of textual BasicBlock order.
  std::vector<llvm::BasicBlock*> ordered;
  std::unordered_set<llvm::BasicBlock*> seen;
  auto* current = header;
  while (current && seen.insert(current).second) {
    ordered.push_back(current);
    if (current == latch)
      break;
    llvm::BasicBlock* next = nullptr;
    for (auto* successor : llvm::successors(current)) {
      if (!loop.contains(successor) || isBackedge(current, successor, loop))
        continue;
      if (next) {
        return fail(LinearLoopStatus::NonLinearCFG,
                    "linear loop path has more than one forward successor");
      }
      next = successor;
    }
    if (!next)
      return fail(LinearLoopStatus::NonLinearCFG,
                  "linear loop path terminates before reaching its latch");
    current = next;
  }
  if (ordered.size() != loop.getBlocks().size() || current != latch)
    return fail(LinearLoopStatus::NonLinearCFG, "in-loop CFG is not one path from header to latch");

  std::unordered_set<llvm::BasicBlock*> members(ordered.begin(), ordered.end());
  for (auto* block : ordered) {
    unsigned inLoopPredecessors = 0;
    for (auto* predecessor : llvm::predecessors(block)) {
      if (!members.contains(predecessor)) {
        if (block != header)
          return fail(LinearLoopStatus::NonLinearCFG,
                      "linear loop contains a non-header side-entry block");
        continue;
      }
      if (isBackedge(predecessor, block, loop))
        continue;
      ++inLoopPredecessors;
    }
    const unsigned expected = block == header ? 0U : 1U;
    if (inLoopPredecessors != expected)
      return fail(LinearLoopStatus::NonLinearCFG,
                  "linear loop path has a fork, merge, or side-entry block");
  }

  LinearLoopRegionDescriptor region;
  region.header = header;
  region.latch = latch;
  region.exiting = exiting;
  region.exit = exit;
  region.preheader = preheader;
  region.terminationBranch = termination;
  region.orderedBlocks = std::move(ordered);
  return LinearLoopAnalysisResult{LinearLoopStatus::Success, {}, std::move(region)};
}

} // namespace cgra::frontend::llvm_frontend
