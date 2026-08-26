// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMMemoryAnalysis.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <tuple>

namespace cgra::frontend::llvm_frontend {
namespace {

using DependenceKey = std::tuple<std::uint32_t, std::uint32_t, ir::MemoryDepKind, std::uint32_t>;

LLVMMemoryAnalysisResult fail(LLVMMemoryAnalysisStatus status, std::string message) {
  LLVMMemoryAnalysisResult result;
  result.status = status;
  result.message = std::move(message);
  return result;
}

const llvm::Value* pointerOperand(const llvm::Instruction& instruction) {
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
    return load->getPointerOperand();
  if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
    return store->getPointerOperand();
  return nullptr;
}

bool isStore(const LLVMMemoryAccessDescriptor& access) {
  return access.kind == LLVMMemoryAccessKind::Store;
}

ir::MemoryDepKind dependenceKind(const LLVMMemoryAccessDescriptor& source,
                                 const LLVMMemoryAccessDescriptor& destination) {
  if (isStore(source) && isStore(destination))
    return ir::MemoryDepKind::WAW;
  if (isStore(source))
    return ir::MemoryDepKind::RAW;
  return ir::MemoryDepKind::WAR;
}

std::optional<std::int64_t> signedConstant(const llvm::SCEV* value) {
  const auto* constant = llvm::dyn_cast<llvm::SCEVConstant>(value);
  if (!constant || constant->getAPInt().getMinSignedBits() > 64)
    return std::nullopt;
  return constant->getAPInt().getSExtValue();
}

struct AffineValue {
  std::int64_t offset = 0;
  std::int64_t stride = 0;
};

bool fitsAddressWord(std::int64_t value) {
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

std::optional<AffineValue> affineValue(const llvm::Value& value, const llvm::Loop& loop,
                                       llvm::ScalarEvolution& scalarEvolution) {
  const auto* expression = scalarEvolution.getSCEV(const_cast<llvm::Value*>(&value));
  if (const auto constant = signedConstant(expression))
    return AffineValue{*constant, 0};
  const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(expression);
  if (!recurrence || recurrence->getLoop() != &loop || !recurrence->isAffine())
    return std::nullopt;
  const auto start = signedConstant(recurrence->getStart());
  const auto step = signedConstant(recurrence->getStepRecurrence(scalarEvolution));
  if (!start || !step)
    return std::nullopt;
  return AffineValue{*start, *step};
}

std::optional<LLVMMemoryAccessDescriptor> analyzeAccess(const llvm::Instruction& instruction,
                                                        const llvm::Loop& loop,
                                                        const llvm::DataLayout& dataLayout,
                                                        llvm::ScalarEvolution& scalarEvolution,
                                                        LLVMMemoryAnalysisResult& error) {
  const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
  const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  if (!load && !store)
    return std::nullopt;

  const auto* valueType = load ? load->getType() : store->getValueOperand()->getType();
  if (!valueType->isIntegerTy(32)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAccessType,
                 "T018 V0 supports only i32 Load/Store accesses");
    return std::nullopt;
  }
  if ((load && (load->isVolatile() || load->isAtomic())) ||
      (store && (store->isVolatile() || store->isAtomic()))) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedVolatileOrAtomic,
                 "volatile and atomic memory operations are outside T018 V0");
    return std::nullopt;
  }
  const auto alignment = load ? load->getAlign().value() : store->getAlign().value();
  if (alignment < 4) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAlignment,
                 "T018 V0 requires naturally aligned i32 memory accesses");
    return std::nullopt;
  }

  const auto* address = pointerOperand(instruction);
  const auto* pointerType = llvm::dyn_cast<llvm::PointerType>(address->getType());
  if (!pointerType || pointerType->getAddressSpace() != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAddressSpace,
                 "T018 V0 supports only the default LLVM address space");
    return std::nullopt;
  }
  const auto* base = llvm::getUnderlyingObject(address);
  if (!base || !base->getType()->isPointerTy() ||
      (llvm::isa<llvm::Instruction>(base) && loop.contains(llvm::cast<llvm::Instruction>(base))) ||
      llvm::isa<llvm::PHINode>(base) || llvm::isa<llvm::SelectInst>(base) ||
      llvm::isa<llvm::LoadInst>(base)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedPointerBase,
                 "memory address must have one loop-invariant pointer root");
    return std::nullopt;
  }

  LLVMMemoryAccessDescriptor descriptor;
  descriptor.kind = load ? LLVMMemoryAccessKind::Load : LLVMMemoryAccessKind::Store;
  descriptor.instruction = &instruction;
  descriptor.address = address;
  descriptor.base = base;
  descriptor.accessWidthBits = 32;
  descriptor.exactAffine = true;

  const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address);
  if (!gep)
    return descriptor;

  const auto bitWidth = dataLayout.getIndexTypeSizeInBits(gep->getType());
  llvm::MapVector<llvm::Value*, llvm::APInt> variableOffsets;
  llvm::APInt constantOffset(bitWidth, 0, true);
  if (!gep->collectOffset(dataLayout, bitWidth, variableOffsets, constantOffset) ||
      constantOffset.getMinSignedBits() > 64 || constantOffset.getSExtValue() % 4 != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "GEP byte offset is not an exact aligned affine word offset");
    return std::nullopt;
  }
  descriptor.gepConstantOffsetWords = constantOffset.getSExtValue() / 4;
  if (!fitsAddressWord(descriptor.gepConstantOffsetWords)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "GEP constant word offset is not representable by Generic i32 address arithmetic");
    return std::nullopt;
  }
  descriptor.constantOffsetWords = descriptor.gepConstantOffsetWords;
  if (variableOffsets.size() > 1) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "T018 V0 accepts one affine dynamic GEP index");
    return std::nullopt;
  }
  if (variableOffsets.empty())
    return descriptor;

  const auto& [index, scaleBytesValue] = variableOffsets.front();
  if (scaleBytesValue.getMinSignedBits() > 64 || scaleBytesValue.getSExtValue() % 4 != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "dynamic GEP scale is not a whole scratchpad word");
    return std::nullopt;
  }
  const auto scaleWords = scaleBytesValue.getSExtValue() / 4;
  if (!fitsAddressWord(scaleWords)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "dynamic GEP word scale is not representable by Generic i32 address arithmetic");
    return std::nullopt;
  }
  const auto affine = affineValue(*index, loop, scalarEvolution);
  if (!affine) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "dynamic GEP index is not an exact affine loop expression");
    return std::nullopt;
  }
  if (!fitsAddressWord(affine->offset) || !fitsAddressWord(affine->stride)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "affine GEP index exceeds the Generic i32 address domain");
    return std::nullopt;
  }
  const auto derivedOffset = descriptor.gepConstantOffsetWords + affine->offset * scaleWords;
  const auto derivedStride = affine->stride * scaleWords;
  if (!fitsAddressWord(derivedOffset) || !fitsAddressWord(derivedStride)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "affine GEP word offset or stride exceeds the Generic i32 address domain");
    return std::nullopt;
  }
  descriptor.dynamicIndex = index;
  descriptor.dynamicScaleWords = scaleWords;
  descriptor.constantOffsetWords = derivedOffset;
  descriptor.iterationStrideWords = derivedStride;
  return descriptor;
}

bool instructionBefore(const llvm::Instruction& lhs, const llvm::Instruction& rhs,
                       const llvm::DominatorTree& dominatorTree) {
  if (lhs.getParent() == rhs.getParent())
    return lhs.comesBefore(&rhs);
  return dominatorTree.dominates(lhs.getParent(), rhs.getParent());
}

void addDependence(LLVMMemoryAnalysisResult& result, std::set<DependenceKey>& seen,
                   std::uint32_t source, std::uint32_t destination, ir::MemoryDepKind kind,
                   std::uint32_t distance, LLVMMemoryDependenceMode mode, std::string reason) {
  if (!seen.emplace(source, destination, kind, distance).second)
    return;
  result.dependences.push_back({source, destination, kind, distance, mode, std::move(reason)});
}

std::optional<std::uint32_t> positiveDistance(std::int64_t numerator, std::int64_t stride) {
  if (stride == 0 || numerator % stride != 0)
    return std::nullopt;
  const auto distance = numerator / stride;
  if (distance <= 0 ||
      static_cast<std::uint64_t>(distance) > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return static_cast<std::uint32_t>(distance);
}

} // namespace

std::string_view toString(LLVMMemoryAnalysisStatus status) noexcept {
  switch (status) {
  case LLVMMemoryAnalysisStatus::Success:
    return "success";
  case LLVMMemoryAnalysisStatus::UnsupportedAccessType:
    return "unsupported_access_type";
  case LLVMMemoryAnalysisStatus::UnsupportedVolatileOrAtomic:
    return "unsupported_volatile_or_atomic";
  case LLVMMemoryAnalysisStatus::UnsupportedAddressSpace:
    return "unsupported_address_space";
  case LLVMMemoryAnalysisStatus::UnsupportedAlignment:
    return "unsupported_alignment";
  case LLVMMemoryAnalysisStatus::UnsupportedPointerBase:
    return "unsupported_pointer_base";
  case LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress:
    return "unsupported_non_affine_address";
  case LLVMMemoryAnalysisStatus::UnsupportedPathSensitiveOrder:
    return "unsupported_path_sensitive_order";
  case LLVMMemoryAnalysisStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(LLVMMemoryAccessKind kind) noexcept {
  return kind == LLVMMemoryAccessKind::Load ? "load" : "store";
}

std::string_view toString(LLVMMemoryDependenceMode mode) noexcept {
  return mode == LLVMMemoryDependenceMode::ExactAffine ? "exact_affine" : "conservative";
}

LLVMMemoryAnalysisResult analyzeMemoryDependences(const llvm::Loop& loop,
                                                  const llvm::DominatorTree& dominatorTree,
                                                  llvm::LoopInfo& loopInfo) {
  auto* function = loop.getHeader()->getParent();
  const auto& dataLayout = function->getParent()->getDataLayout();
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(*function);
  llvm::ScalarEvolution scalarEvolution(*function, libraryInfo, assumptions,
                                        const_cast<llvm::DominatorTree&>(dominatorTree), loopInfo);
  llvm::BasicAAResult basicAA(dataLayout, *function, libraryInfo, assumptions,
                              const_cast<llvm::DominatorTree*>(&dominatorTree));
  llvm::AAResults aliasAnalysis(libraryInfo);
  aliasAnalysis.addAAResult(basicAA);

  LLVMMemoryAnalysisResult result;
  result.status = LLVMMemoryAnalysisStatus::Success;
  for (auto& block : *function) {
    if (!loop.contains(&block))
      continue;
    for (const auto& instruction : block) {
      if (!llvm::isa<llvm::LoadInst>(instruction) && !llvm::isa<llvm::StoreInst>(instruction))
        continue;
      LLVMMemoryAnalysisResult error;
      auto descriptor = analyzeAccess(instruction, loop, dataLayout, scalarEvolution, error);
      if (!descriptor)
        return error;
      descriptor->id = static_cast<std::uint32_t>(result.accesses.size());
      result.accesses.push_back(*descriptor);
    }
  }

  std::set<DependenceKey> seen;
  for (const auto& access : result.accesses) {
    if (isStore(access) && access.iterationStrideWords == 0)
      addDependence(result, seen, access.id, access.id, ir::MemoryDepKind::WAW, 1,
                    LLVMMemoryDependenceMode::ExactAffine,
                    "invariant Store repeats the same logical word");
  }

  for (std::size_t lhsIndex = 0; lhsIndex < result.accesses.size(); ++lhsIndex) {
    for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < result.accesses.size(); ++rhsIndex) {
      auto* lhs = &result.accesses[lhsIndex];
      auto* rhs = &result.accesses[rhsIndex];
      if (!isStore(*lhs) && !isStore(*rhs))
        continue;

      const bool exact = lhs->exactAffine && rhs->exactAffine && lhs->base == rhs->base &&
                         lhs->accessWidthBits == rhs->accessWidthBits &&
                         lhs->iterationStrideWords == rhs->iterationStrideWords;
      if (!exact && aliasAnalysis.alias(llvm::MemoryLocation::get(lhs->instruction),
                                        llvm::MemoryLocation::get(rhs->instruction)) ==
                        llvm::AliasResult::NoAlias)
        continue;

      if (!instructionBefore(*lhs->instruction, *rhs->instruction, dominatorTree)) {
        if (instructionBefore(*rhs->instruction, *lhs->instruction, dominatorTree))
          std::swap(lhs, rhs);
        else
          return fail(LLVMMemoryAnalysisStatus::UnsupportedPathSensitiveOrder,
                      "MayAlias accesses have no path-independent program order");
      }

      if (exact) {
        const auto stride = lhs->iterationStrideWords;
        if (lhs->constantOffsetWords == rhs->constantOffsetWords)
          addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), 0,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "same affine address in the same iteration");
        if (stride == 0) {
          if (lhs->constantOffsetWords == rhs->constantOffsetWords)
            addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), 1,
                          LLVMMemoryDependenceMode::ExactAffine,
                          "invariant address dynamic order across iterations");
          continue;
        }
        if (const auto distance =
                positiveDistance(lhs->constantOffsetWords - rhs->constantOffsetWords, stride))
          addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), *distance,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "exact positive affine dependence distance");
        if (const auto distance =
                positiveDistance(rhs->constantOffsetWords - lhs->constantOffsetWords, stride))
          addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), *distance,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "exact reverse positive affine dependence distance");
        continue;
      }

      addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), 0,
                    LLVMMemoryDependenceMode::Conservative,
                    "MayAlias program order within one iteration");
      addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), 1,
                    LLVMMemoryDependenceMode::Conservative,
                    "MayAlias reverse order across iterations");
    }
  }
  return result;
}

} // namespace cgra::frontend::llvm_frontend
