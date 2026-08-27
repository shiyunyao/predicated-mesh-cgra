// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class BasicBlock;
class BranchInst;
class Loop;
} // namespace llvm

namespace cgra::frontend::llvm_frontend {

enum class LinearLoopStatus {
  Success,
  NoPreheader,
  NoLatch,
  ExitShape,
  InternalConditionalBranch,
  UnsupportedTerminator,
  NonLinearCFG,
  NonHeaderPHI,
};

std::string_view toString(LinearLoopStatus status) noexcept;

struct LinearLoopRegionDescriptor {
  llvm::BasicBlock* header = nullptr;
  llvm::BasicBlock* latch = nullptr;
  llvm::BasicBlock* exiting = nullptr;
  llvm::BasicBlock* exit = nullptr;
  llvm::BasicBlock* preheader = nullptr;
  llvm::BranchInst* terminationBranch = nullptr;
  std::vector<llvm::BasicBlock*> orderedBlocks;
};

struct LinearLoopAnalysisResult {
  LinearLoopStatus status = LinearLoopStatus::Success;
  std::string message;
  std::optional<LinearLoopRegionDescriptor> region;

  bool ok() const noexcept { return status == LinearLoopStatus::Success && region.has_value(); }
};

LinearLoopAnalysisResult discoverLinearLoopRegion(llvm::Loop& loop);

} // namespace cgra::frontend::llvm_frontend
