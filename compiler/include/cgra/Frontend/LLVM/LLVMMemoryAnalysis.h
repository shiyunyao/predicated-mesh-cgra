// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/Edge.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class DominatorTree;
class Instruction;
class Loop;
class LoopInfo;
class Value;
} // namespace llvm

namespace cgra::frontend::llvm_frontend {

enum class LLVMMemoryAnalysisStatus {
  Success,
  UnsupportedAccessType,
  UnsupportedVolatileOrAtomic,
  UnsupportedAddressSpace,
  UnsupportedAlignment,
  UnsupportedPointerBase,
  UnsupportedNonAffineAddress,
  UnsupportedPathSensitiveOrder,
  InternalError,
};

std::string_view toString(LLVMMemoryAnalysisStatus status) noexcept;

enum class LLVMMemoryAccessKind { Load, Store };
std::string_view toString(LLVMMemoryAccessKind kind) noexcept;

enum class LLVMMemoryDependenceMode { ExactAffine, Conservative };
std::string_view toString(LLVMMemoryDependenceMode mode) noexcept;

struct LLVMMemoryAccessDescriptor {
  std::uint32_t id = 0;
  LLVMMemoryAccessKind kind = LLVMMemoryAccessKind::Load;
  const llvm::Instruction* instruction = nullptr;
  const llvm::Value* address = nullptr;
  const llvm::Value* base = nullptr;
  const llvm::Value* dynamicIndex = nullptr;
  std::int64_t dynamicScaleWords = 0;
  std::int64_t gepConstantOffsetWords = 0;
  std::int64_t constantOffsetWords = 0;
  std::int64_t iterationStrideWords = 0;
  std::uint32_t accessWidthBits = 0;
  bool exactAffine = false;
};

struct LLVMMemoryDependenceDescriptor {
  std::uint32_t sourceAccess = 0;
  std::uint32_t destinationAccess = 0;
  ir::MemoryDepKind kind = ir::MemoryDepKind::RAW;
  std::uint32_t distance = 0;
  LLVMMemoryDependenceMode mode = LLVMMemoryDependenceMode::ExactAffine;
  std::string reason;
};

struct LLVMMemoryAnalysisResult {
  LLVMMemoryAnalysisStatus status = LLVMMemoryAnalysisStatus::InternalError;
  std::string message;
  std::vector<LLVMMemoryAccessDescriptor> accesses;
  std::vector<LLVMMemoryDependenceDescriptor> dependences;

  bool ok() const noexcept { return status == LLVMMemoryAnalysisStatus::Success; }
};

LLVMMemoryAnalysisResult analyzeMemoryDependences(const llvm::Loop& loop,
                                                  const llvm::DominatorTree& dominatorTree,
                                                  llvm::LoopInfo& loopInfo);

} // namespace cgra::frontend::llvm_frontend
