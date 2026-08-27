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
#include <llvm/Support/raw_ostream.h>

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
  LLVMAddressMode mode = LLVMAddressMode::ExactAffine;
  std::string invariantExpression;
};

bool fitsAddressWord(std::int64_t value) {
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

bool addScaled(std::int64_t& accumulator, std::int64_t value, std::int64_t scale) {
  std::int64_t product = 0;
  std::int64_t sum = 0;
  if (__builtin_mul_overflow(value, scale, &product) ||
      __builtin_add_overflow(accumulator, product, &sum))
    return false;
  accumulator = sum;
  return true;
}

struct CollectedAddress {
  const llvm::Value* base = nullptr;
  std::int64_t constantBytes = 0;
  std::vector<std::pair<const llvm::Value*, std::int64_t>> terms;
};

struct PointerRoot {
  const llvm::Value* base = nullptr;
  const llvm::PHINode* phi = nullptr;
  const llvm::PHINode* mergePhi = nullptr;
  const llvm::GetElementPtrInst* backedge = nullptr;
  std::int64_t stepBytes = 0;
};

std::optional<CollectedAddress> collectAddress(const llvm::Value& address,
                                               const llvm::DataLayout& dataLayout,
                                               std::string& error) {
  std::vector<const llvm::GetElementPtrInst*> chain;
  const llvm::Value* cursor = &address;
  while (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(cursor)) {
    chain.push_back(gep);
    cursor = gep->getPointerOperand()->stripPointerCasts();
  }
  if (chain.empty())
    return CollectedAddress{cursor, 0, {}};
  std::ranges::reverse(chain);

  CollectedAddress result;
  result.base = cursor;
  llvm::MapVector<const llvm::Value*, std::int64_t> combinedTerms;
  for (const auto* gep : chain) {
    const auto bitWidth = dataLayout.getIndexTypeSizeInBits(gep->getType());
    llvm::MapVector<llvm::Value*, llvm::APInt> offsets;
    llvm::APInt constant(bitWidth, 0, true);
    if (!gep->collectOffset(dataLayout, bitWidth, offsets, constant) ||
        constant.getMinSignedBits() > 64 || !addScaled(result.constantBytes, 1,
                                                       constant.getSExtValue())) {
      error = "GEP chain byte offset is not representable as signed 64-bit";
      return std::nullopt;
    }
    for (const auto& [value, scale] : offsets) {
      if (scale.getMinSignedBits() > 64) {
        error = "GEP chain scale is not representable as signed 64-bit";
        return std::nullopt;
      }
      auto& combined = combinedTerms[value];
      if (!addScaled(combined, 1, scale.getSExtValue())) {
        error = "combined GEP chain scale overflows signed 64-bit";
        return std::nullopt;
      }
    }
  }
  for (const auto& [value, scale] : combinedTerms)
    if (scale != 0)
      result.terms.emplace_back(value, scale);
  return result;
}

std::optional<PointerRoot> resolvePointerRoot(const llvm::Value& root, const llvm::Loop& loop,
                                              const llvm::DataLayout& dataLayout,
                                              std::string& error) {
  const auto* phi = llvm::dyn_cast<llvm::PHINode>(&root);
  if (!phi) {
    const auto* base = llvm::getUnderlyingObject(&root);
    if (!base || !base->getType()->isPointerTy() ||
        (llvm::isa<llvm::Instruction>(base) &&
         loop.contains(llvm::cast<llvm::Instruction>(base))) ||
        llvm::isa<llvm::PHINode>(base) || llvm::isa<llvm::SelectInst>(base) ||
        llvm::isa<llvm::LoadInst>(base)) {
      error = "memory address must have one loop-invariant pointer root";
      return std::nullopt;
    }
    return PointerRoot{base, nullptr, nullptr, nullptr, 0};
  }

  const auto* preheader = loop.getLoopPreheader();
  const auto* latch = loop.getLoopLatch();
  if (phi->getParent() != loop.getHeader() || !preheader || !latch ||
      phi->getNumIncomingValues() != 2) {
    error = "pointer PHI must be a canonical selected-loop recurrence";
    return std::nullopt;
  }
  const int initialIndex = phi->getBasicBlockIndex(preheader);
  const int backedgeIndex = phi->getBasicBlockIndex(latch);
  if (initialIndex < 0 || backedgeIndex < 0) {
    error = "pointer PHI must have one preheader and one latch incoming value";
    return std::nullopt;
  }
  const auto* initial = phi->getIncomingValue(static_cast<unsigned>(initialIndex))->stripPointerCasts();
  const auto* initialBase = llvm::getUnderlyingObject(initial);
  if (!initialBase || initialBase != initial || !initialBase->getType()->isPointerTy() ||
      (llvm::isa<llvm::Instruction>(initialBase) &&
       loop.contains(llvm::cast<llvm::Instruction>(initialBase)))) {
    error = "pointer recurrence initial value must be one loop-external pointer root";
    return std::nullopt;
  }
  const auto* latchValue =
      phi->getIncomingValue(static_cast<unsigned>(backedgeIndex))->stripPointerCasts();
  const auto* mergePhi = llvm::dyn_cast<llvm::PHINode>(latchValue);
  const auto* backedge = llvm::dyn_cast<llvm::GetElementPtrInst>(latchValue);
  if (mergePhi) {
    if (!loop.contains(mergePhi) || mergePhi->getNumIncomingValues() != 2) {
      error = "conditional pointer recurrence must have one two-way merge PHI";
      return std::nullopt;
    }
    for (const auto& incoming : mergePhi->incoming_values()) {
      const auto* value = incoming.get()->stripPointerCasts();
      if (value == phi)
        continue;
      const auto* candidate = llvm::dyn_cast<llvm::GetElementPtrInst>(value);
      if (candidate && candidate->getPointerOperand()->stripPointerCasts() == phi && !backedge) {
        backedge = candidate;
        continue;
      }
      error = "conditional pointer recurrence must select the prior pointer or one GEP update";
      return std::nullopt;
    }
  }
  if (!backedge || !loop.contains(backedge) ||
      backedge->getPointerOperand()->stripPointerCasts() != phi) {
    error = "pointer recurrence latch value must be a direct or selected constant-step GEP";
    return std::nullopt;
  }
  std::string collectionError;
  const auto update = collectAddress(*backedge, dataLayout, collectionError);
  if (!update || update->base != phi || !update->terms.empty() || update->constantBytes == 0) {
    error = collectionError.empty()
                ? "pointer recurrence GEP must have one non-zero constant byte step"
                : std::move(collectionError);
    return std::nullopt;
  }
  return PointerRoot{initialBase, phi, mergePhi, backedge, update->constantBytes};
}

std::optional<AffineValue> affineValue(const llvm::Value& value, const llvm::Loop& loop,
                                       llvm::ScalarEvolution& scalarEvolution) {
  const auto* expression = scalarEvolution.getSCEV(const_cast<llvm::Value*>(&value));
  if (const auto constant = signedConstant(expression))
    return AffineValue{*constant, 0, LLVMAddressMode::ExactAffine, {}};
  if (scalarEvolution.isLoopInvariant(expression, &loop)) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    expression->print(stream);
    stream.flush();
    return AffineValue{0, 0, LLVMAddressMode::SymbolicAffine, std::move(text)};
  }
  const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(expression);
  if (recurrence && recurrence->getLoop() == &loop && recurrence->isAffine()) {
    const auto step = signedConstant(recurrence->getStepRecurrence(scalarEvolution));
    if (!step)
      return AffineValue{0, 0, LLVMAddressMode::Dynamic, {}};
    if (const auto start = signedConstant(recurrence->getStart()))
      return AffineValue{*start, *step, LLVMAddressMode::ExactAffine, {}};
    if (scalarEvolution.isLoopInvariant(recurrence->getStart(), &loop)) {
      std::string text;
      llvm::raw_string_ostream stream(text);
      recurrence->getStart()->print(stream);
      stream.flush();
      return AffineValue{0, *step, LLVMAddressMode::SymbolicAffine, std::move(text)};
    }
  }
  return AffineValue{0, 0, LLVMAddressMode::Dynamic, {}};
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
  const bool scalarMemoryType = valueType->isIntegerTy() || valueType->isFloatTy() ||
                                valueType->isDoubleTy();
  const auto accessWidthBits =
      scalarMemoryType ? static_cast<std::uint32_t>(valueType->getPrimitiveSizeInBits()) : 0U;
  if (!scalarMemoryType || (accessWidthBits != 8 && accessWidthBits != 16 &&
                            accessWidthBits != 32 && accessWidthBits != 64)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAccessType,
                 "mapping frontend supports scalar 8/16/32/64-bit integer and float accesses");
    return std::nullopt;
  }
  if ((load && (load->isVolatile() || load->isAtomic())) ||
      (store && (store->isVolatile() || store->isAtomic()))) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedVolatileOrAtomic,
                 "volatile and atomic memory operations are outside T018 V0");
    return std::nullopt;
  }
  const auto alignment = load ? load->getAlign().value() : store->getAlign().value();
  const auto naturalAlignmentBytes = accessWidthBits / 8;
  if (alignment < naturalAlignmentBytes) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAlignment,
                 "memory access does not satisfy its natural scalar alignment");
    return std::nullopt;
  }

  const auto* address = pointerOperand(instruction);
  const auto* pointerType = llvm::dyn_cast<llvm::PointerType>(address->getType());
  if (!pointerType || pointerType->getAddressSpace() != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedAddressSpace,
                 "T018 V0 supports only the default LLVM address space");
    return std::nullopt;
  }
  std::string collectionError;
  const auto collected = collectAddress(*address, dataLayout, collectionError);
  if (!collected) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 std::move(collectionError));
    return std::nullopt;
  }
  std::string rootError;
  const auto pointerRoot = resolvePointerRoot(*collected->base, loop, dataLayout, rootError);
  if (!pointerRoot) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedPointerBase, std::move(rootError));
    return std::nullopt;
  }

  LLVMMemoryAccessDescriptor descriptor;
  descriptor.kind = load ? LLVMMemoryAccessKind::Load : LLVMMemoryAccessKind::Store;
  descriptor.instruction = &instruction;
  descriptor.address = address;
  descriptor.base = pointerRoot->base;
  descriptor.addressRoot = collected->base;
  descriptor.pointerPhi = pointerRoot->phi;
  descriptor.pointerMergePhi = pointerRoot->mergePhi;
  descriptor.pointerBackedge = pointerRoot->backedge;
  descriptor.pointerStepBytes = pointerRoot->stepBytes;
  descriptor.accessWidthBits = accessWidthBits;
  descriptor.exactAffine = true;
  const std::int64_t addressUnitBytes = accessWidthBits < 32 ? 1 : 4;
  if (pointerRoot->stepBytes % addressUnitBytes != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "pointer recurrence step is not a whole Generic address unit");
    return std::nullopt;
  }
  descriptor.pointerStepWords = pointerRoot->stepBytes / addressUnitBytes;
  descriptor.iterationStrideBytes = pointerRoot->stepBytes;
  descriptor.iterationStrideWords = descriptor.pointerStepWords;
  if (pointerRoot->mergePhi) {
    descriptor.addressMode = LLVMAddressMode::Dynamic;
    descriptor.exactAffine = false;
    descriptor.iterationStrideBytes = 0;
    descriptor.iterationStrideWords = 0;
  }
  if (collected->constantBytes % addressUnitBytes != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "GEP byte offset is not aligned to the Generic address unit");
    return std::nullopt;
  }
  descriptor.gepConstantOffsetWords = collected->constantBytes / addressUnitBytes;
  descriptor.gepConstantOffsetBytes = collected->constantBytes;
  if (!fitsAddressWord(descriptor.gepConstantOffsetWords)) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "GEP constant word offset is not representable by Generic i32 address arithmetic");
    return std::nullopt;
  }
  descriptor.constantOffsetWords = descriptor.gepConstantOffsetWords;
  descriptor.constantOffsetBytes = descriptor.gepConstantOffsetBytes;
  if (collected->terms.empty())
    return descriptor;

  descriptor.constantOffsetWords = descriptor.gepConstantOffsetWords;
  descriptor.constantOffsetBytes = descriptor.gepConstantOffsetBytes;
  if (!pointerRoot->mergePhi)
    descriptor.addressMode = LLVMAddressMode::ExactAffine;
  std::vector<std::string> invariantTerms;
  for (const auto& [index, scaleBytes] : collected->terms) {
    if (scaleBytes % addressUnitBytes != 0) {
      error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                   "dynamic GEP scale is not a whole Generic address unit");
      return std::nullopt;
    }
    const auto scaleWords = scaleBytes / addressUnitBytes;
    if (!fitsAddressWord(scaleWords)) {
      error = fail(
          LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
          "dynamic GEP scale is not representable by Generic address arithmetic");
      return std::nullopt;
    }
    descriptor.dynamicTerms.push_back({index, scaleBytes, scaleWords});
    const auto affine = affineValue(*index, loop, scalarEvolution);
    if (!affine || !fitsAddressWord(affine->offset) || !fitsAddressWord(affine->stride) ||
        !addScaled(descriptor.constantOffsetWords, affine ? affine->offset : 0, scaleWords) ||
        !addScaled(descriptor.iterationStrideWords, affine ? affine->stride : 0, scaleWords) ||
        !addScaled(descriptor.constantOffsetBytes, affine ? affine->offset : 0, scaleBytes) ||
        !addScaled(descriptor.iterationStrideBytes, affine ? affine->stride : 0, scaleBytes)) {
      error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                   "GEP affine term exceeds the signed Generic address domain");
      return std::nullopt;
    }
    if (affine->mode == LLVMAddressMode::Dynamic)
      descriptor.addressMode = LLVMAddressMode::Dynamic;
    else if (affine->mode == LLVMAddressMode::SymbolicAffine &&
             descriptor.addressMode != LLVMAddressMode::Dynamic) {
      descriptor.addressMode = LLVMAddressMode::SymbolicAffine;
      invariantTerms.push_back(std::to_string(scaleBytes) + "*(" +
                               affine->invariantExpression + ")");
    }
  }
  if (descriptor.dynamicTerms.size() == 1) {
    descriptor.dynamicIndex = descriptor.dynamicTerms.front().value;
    descriptor.dynamicScaleBytes = descriptor.dynamicTerms.front().scaleBytes;
    descriptor.dynamicScaleWords = descriptor.dynamicTerms.front().scaleWords;
  }
  std::ranges::sort(invariantTerms);
  for (const auto& term : invariantTerms) {
    if (!descriptor.invariantExpression.empty())
      descriptor.invariantExpression += "+";
    descriptor.invariantExpression += term;
  }
  descriptor.exactAffine = descriptor.addressMode != LLVMAddressMode::Dynamic;
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

std::string_view toString(LLVMAddressMode mode) noexcept {
  switch (mode) {
  case LLVMAddressMode::ExactAffine:
    return "exact_affine";
  case LLVMAddressMode::SymbolicAffine:
    return "symbolic_affine";
  case LLVMAddressMode::Dynamic:
    return "dynamic";
  }
  return "dynamic";
}

std::string_view toString(LLVMMemoryDependenceMode mode) noexcept {
  switch (mode) {
  case LLVMMemoryDependenceMode::ExactAffine:
    return "exact_affine";
  case LLVMMemoryDependenceMode::Conservative:
    return "conservative";
  case LLVMMemoryDependenceMode::DynamicConservative:
    return "dynamic_conservative";
  }
  return "dynamic_conservative";
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
    if (isStore(access) && access.addressMode == LLVMAddressMode::Dynamic)
      addDependence(result, seen, access.id, access.id, ir::MemoryDepKind::WAW, 1,
                    LLVMMemoryDependenceMode::DynamicConservative,
                    "dynamic Store may revisit an address in the next iteration");
    else if (isStore(access) && access.iterationStrideBytes == 0)
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
                         lhs->iterationStrideBytes == rhs->iterationStrideBytes &&
                         lhs->invariantExpression == rhs->invariantExpression;
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
        const auto stride = lhs->iterationStrideBytes;
        if (lhs->constantOffsetBytes == rhs->constantOffsetBytes)
          addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), 0,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "same affine address in the same iteration");
        if (stride == 0) {
          if (lhs->constantOffsetBytes == rhs->constantOffsetBytes)
            addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), 1,
                          LLVMMemoryDependenceMode::ExactAffine,
                          "invariant address dynamic order across iterations");
          continue;
        }
        if (const auto distance =
                positiveDistance(lhs->constantOffsetBytes - rhs->constantOffsetBytes, stride))
          addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), *distance,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "exact positive affine dependence distance");
        if (const auto distance =
                positiveDistance(rhs->constantOffsetBytes - lhs->constantOffsetBytes, stride))
          addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), *distance,
                        LLVMMemoryDependenceMode::ExactAffine,
                        "exact reverse positive affine dependence distance");
        continue;
      }

      const auto conservativeMode =
          lhs->addressMode == LLVMAddressMode::Dynamic || rhs->addressMode == LLVMAddressMode::Dynamic
              ? LLVMMemoryDependenceMode::DynamicConservative
              : LLVMMemoryDependenceMode::Conservative;
      addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), 0,
                    conservativeMode,
                    "MayAlias program order within one iteration");
      addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), 1,
                    conservativeMode,
                    "MayAlias reverse order across iterations");
    }
  }
  return result;
}

} // namespace cgra::frontend::llvm_frontend
