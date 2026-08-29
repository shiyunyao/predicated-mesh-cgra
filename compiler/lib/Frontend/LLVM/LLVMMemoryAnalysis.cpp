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
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <unordered_map>
#include <tuple>
#include <unordered_set>

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
  bool dynamic = false;
};

const llvm::Value* stripPointerCastsPreservingGEPs(const llvm::Value* value) {
  while (const auto* cast = llvm::dyn_cast_or_null<llvm::CastInst>(value)) {
    if (!llvm::isa<llvm::BitCastInst, llvm::AddrSpaceCastInst>(cast))
      break;
    value = cast->getOperand(0);
  }
  return value;
}

const llvm::PHINode* findPointerRecurrencePhi(const llvm::Value* value, const llvm::Loop& loop,
                                              std::unordered_set<const llvm::Value*>& visited) {
  value = stripPointerCastsPreservingGEPs(value);
  if (!value || !visited.insert(value).second)
    return nullptr;
  if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (phi->getParent() == loop.getHeader() && phi->getType()->isPointerTy())
      return phi;
    const llvm::PHINode* result = nullptr;
    for (const auto& incoming : phi->incoming_values()) {
      const auto* candidate = findPointerRecurrencePhi(incoming.get(), loop, visited);
      if (candidate && result && candidate != result)
        return nullptr;
      if (candidate)
        result = candidate;
    }
    return result;
  }
  if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
    return findPointerRecurrencePhi(gep->getPointerOperand(), loop, visited);
  if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    const auto* truePhi = findPointerRecurrencePhi(select->getTrueValue(), loop, visited);
    const auto* falsePhi = findPointerRecurrencePhi(select->getFalseValue(), loop, visited);
    return truePhi && falsePhi && truePhi != falsePhi ? nullptr : (truePhi ? truePhi : falsePhi);
  }
  return nullptr;
}

bool validPointerRecurrenceExpression(const llvm::Value* value, const llvm::PHINode* phi,
                                      const llvm::Value* initialBase, const llvm::Loop& loop,
                                      std::unordered_set<const llvm::Value*>& visited) {
  value = stripPointerCastsPreservingGEPs(value);
  if (!value)
    return false;
  if (value == phi || value == initialBase)
    return true;
  if (!visited.insert(value).second)
    return true;
  if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
    return loop.contains(gep) && validPointerRecurrenceExpression(gep->getPointerOperand(), phi,
                                                                  initialBase, loop, visited);
  if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
    return loop.contains(select) &&
           validPointerRecurrenceExpression(select->getTrueValue(), phi, initialBase, loop,
                                            visited) &&
           validPointerRecurrenceExpression(select->getFalseValue(), phi, initialBase, loop,
                                            visited);
  if (const auto* merge = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (!loop.contains(merge) || merge == phi)
      return false;
    for (const auto& incoming : merge->incoming_values())
      if (!validPointerRecurrenceExpression(incoming.get(), phi, initialBase, loop, visited))
        return false;
    return true;
  }
  return false;
}

std::optional<CollectedAddress>
collectAddress(const llvm::Value& address, const llvm::DataLayout& dataLayout, std::string& error) {
  std::vector<const llvm::GetElementPtrInst*> chain;
  const llvm::Value* cursor = &address;
  while (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(cursor)) {
    chain.push_back(gep);
    cursor = stripPointerCastsPreservingGEPs(gep->getPointerOperand());
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
        constant.getMinSignedBits() > 64 ||
        !addScaled(result.constantBytes, 1, constant.getSExtValue())) {
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
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&root);
        load && loop.contains(load) && load->getType()->isPointerTy() && !load->isVolatile() &&
        !load->isAtomic())
      return PointerRoot{load, nullptr, nullptr, nullptr, 0, true};
    if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(&root);
        instruction && loop.contains(instruction) && root.getType()->isPointerTy()) {
      std::unordered_set<const llvm::Value*> visited;
      if (const auto* recurrence = findPointerRecurrencePhi(&root, loop, visited))
        return resolvePointerRoot(*recurrence, loop, dataLayout, error);
    }
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
  const auto* initial =
      phi->getIncomingValue(static_cast<unsigned>(initialIndex))->stripPointerCasts();
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
  if (!mergePhi && !backedge) {
    std::unordered_set<const llvm::Value*> visited;
    if (validPointerRecurrenceExpression(latchValue, phi, initialBase, loop, visited))
      return PointerRoot{initialBase, phi, nullptr, nullptr, 0, true};
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

std::optional<LLVMMemoryAccessDescriptor>
analyzeAccess(const llvm::Instruction& instruction, const llvm::Loop& loop,
              const llvm::DataLayout& dataLayout, llvm::ScalarEvolution& scalarEvolution,
              std::uint32_t addressUnitBytes, LLVMMemoryAnalysisResult& error) {
  const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
  const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  if (!load && !store)
    return std::nullopt;

  const auto* valueType = load ? load->getType() : store->getValueOperand()->getType();
  const bool scalarMemoryType = valueType->isIntegerTy() || valueType->isFloatTy() ||
                                valueType->isDoubleTy() || valueType->isPointerTy();
  const auto accessWidthBits =
      valueType->isPointerTy() ? static_cast<std::uint32_t>(dataLayout.getPointerSizeInBits(
                                     llvm::cast<llvm::PointerType>(valueType)->getAddressSpace()))
      : scalarMemoryType       ? static_cast<std::uint32_t>(valueType->getPrimitiveSizeInBits())
                               : 0U;
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
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress, std::move(collectionError));
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
  descriptor.alignmentBytes = static_cast<std::uint32_t>(alignment);
  if (pointerRoot->dynamic && llvm::isa<llvm::LoadInst>(pointerRoot->base))
    descriptor.pointerDomain = LLVMLogicalPointerDomain::LoadedLogicalAddress;
  else if (pointerRoot->phi || collected->base != pointerRoot->base ||
           collected->constantBytes != 0 || !collected->terms.empty())
    descriptor.pointerDomain = LLVMLogicalPointerDomain::ScratchpadOffset;
  else
    descriptor.pointerDomain = LLVMLogicalPointerDomain::ScratchpadBase;
  descriptor.exactAffine = true;
  if (addressUnitBytes == 0 || pointerRoot->stepBytes % addressUnitBytes != 0) {
    error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
                 "pointer recurrence step is not a whole Generic address unit");
    return std::nullopt;
  }
  descriptor.pointerStepWords = pointerRoot->stepBytes / addressUnitBytes;
  descriptor.iterationStrideBytes = pointerRoot->stepBytes;
  descriptor.iterationStrideWords = descriptor.pointerStepWords;
  if (pointerRoot->mergePhi || pointerRoot->dynamic) {
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
  if (!pointerRoot->mergePhi && !pointerRoot->dynamic)
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
      error = fail(LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress,
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
      invariantTerms.push_back(std::to_string(scaleBytes) + "*(" + affine->invariantExpression +
                               ")");
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

std::unordered_set<const llvm::Instruction*> controlOnlyLoads(const llvm::Loop& loop) {
  std::unordered_set<const llvm::Instruction*> slice;
  std::vector<const llvm::Value*> worklist;
  for (const auto* block : loop.blocks()) {
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isConditional())
      continue;
    const bool exitsLoop =
        !loop.contains(branch->getSuccessor(0)) || !loop.contains(branch->getSuccessor(1));
    if (exitsLoop)
      worklist.push_back(branch->getCondition());
  }
  while (!worklist.empty()) {
    const auto* value = worklist.back();
    worklist.pop_back();
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || !loop.contains(instruction) || !slice.insert(instruction).second)
      continue;
    for (const auto& operand : instruction->operands())
      worklist.push_back(operand.get());
  }

  std::unordered_set<const llvm::Instruction*> result;
  for (const auto* instruction : slice) {
    if (!llvm::isa<llvm::LoadInst>(instruction))
      continue;
    const bool hasSemanticUse = std::ranges::any_of(instruction->users(), [&](const auto* user) {
      const auto* use = llvm::dyn_cast<llvm::Instruction>(user);
      return !use || !loop.contains(use) || !slice.contains(use);
    });
    if (!hasSemanticUse)
      result.insert(instruction);
  }
  return result;
}

bool reachesWithinIteration(const llvm::BasicBlock& source, const llvm::BasicBlock& destination,
                            const llvm::Loop& loop) {
  std::vector<const llvm::BasicBlock*> worklist{&source};
  std::unordered_set<const llvm::BasicBlock*> visited{&source};
  while (!worklist.empty()) {
    const auto* block = worklist.back();
    worklist.pop_back();
    for (const auto* successor : llvm::successors(block)) {
      if (!loop.contains(successor) || successor == loop.getHeader())
        continue;
      if (successor == &destination)
        return true;
      if (visited.insert(successor).second)
        worklist.push_back(successor);
    }
  }
  return false;
}

bool instructionBefore(const llvm::Instruction& lhs, const llvm::Instruction& rhs,
                       const llvm::DominatorTree& dominatorTree, const llvm::Loop& loop) {
  if (lhs.getParent() == rhs.getParent())
    return lhs.comesBefore(&rhs);
  return dominatorTree.dominates(lhs.getParent(), rhs.getParent()) ||
         reachesWithinIteration(*lhs.getParent(), *rhs.getParent(), loop);
}

// A reducible loop body is a DAG once backedges to the header are removed.  A
// stable topological order gives conservative MayAlias accesses in mutually
// exclusive arms a deterministic total order without pretending either arm is
// guaranteed to execute.
std::optional<std::unordered_map<const llvm::BasicBlock*, std::size_t>>
conservativeBlockOrder(const llvm::Loop& loop) {
  std::unordered_map<const llvm::BasicBlock*, std::size_t> cfgOrdinal;
  std::vector<const llvm::BasicBlock*> worklist{loop.getHeader()};
  while (!worklist.empty()) {
    const auto* block = worklist.back();
    worklist.pop_back();
    if (!loop.contains(block) || cfgOrdinal.contains(block))
      continue;
    cfgOrdinal.emplace(block, cfgOrdinal.size());
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch)
      continue;
    // Push in reverse so successor 0, whose index carries branch polarity, is
    // visited first. This is independent of textual block placement/names.
    for (unsigned index = branch->getNumSuccessors(); index > 0; --index) {
      const auto* successor = branch->getSuccessor(index - 1);
      if (successor != loop.getHeader() && loop.contains(successor))
        worklist.push_back(successor);
    }
  }
  if (cfgOrdinal.size() != loop.getNumBlocks())
    return std::nullopt;

  std::unordered_map<const llvm::BasicBlock*, std::size_t> indegree;
  for (const auto* block : loop.blocks())
    indegree.emplace(block, 0);
  for (const auto* block : loop.blocks()) {
    for (const auto* successor : llvm::successors(block)) {
      if (!loop.contains(successor) || successor == loop.getHeader())
        continue;
      ++indegree[successor];
    }
  }
  std::set<std::pair<std::size_t, const llvm::BasicBlock*>> ready;
  for (const auto& [block, degree] : indegree)
    if (degree == 0)
      ready.emplace(cfgOrdinal.at(block), block);

  std::unordered_map<const llvm::BasicBlock*, std::size_t> order;
  while (!ready.empty()) {
    const auto [_, block] = *ready.begin();
    ready.erase(ready.begin());
    order.emplace(block, order.size());
    for (const auto* successor : llvm::successors(block)) {
      if (!loop.contains(successor) || successor == loop.getHeader())
        continue;
      auto& degree = indegree.at(successor);
      if (--degree == 0)
        ready.emplace(cfgOrdinal.at(successor), successor);
    }
  }
  if (order.size() != indegree.size())
    return std::nullopt;
  return order;
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

std::string_view toString(LLVMLogicalPointerDomain domain) noexcept {
  switch (domain) {
  case LLVMLogicalPointerDomain::Unknown:
    return "unknown";
  case LLVMLogicalPointerDomain::ScratchpadBase:
    return "scratchpad_base";
  case LLVMLogicalPointerDomain::ScratchpadOffset:
    return "scratchpad_offset";
  case LLVMLogicalPointerDomain::LoadedLogicalAddress:
    return "loaded_logical_scratchpad_address";
  }
  return "unknown";
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
                                                  llvm::LoopInfo& loopInfo,
                                                  std::uint32_t addressUnitBytes) {
  if (addressUnitBytes == 0) {
    return fail(LLVMMemoryAnalysisStatus::InternalError,
                "Generic address unit must be a positive byte count");
  }
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
  const auto ignoredControlLoads = controlOnlyLoads(loop);
  const auto blockOrder = conservativeBlockOrder(loop);
  for (auto& block : *function) {
    if (!loop.contains(&block))
      continue;
    for (const auto& instruction : block) {
      if (!llvm::isa<llvm::LoadInst>(instruction) && !llvm::isa<llvm::StoreInst>(instruction))
        continue;
      if (ignoredControlLoads.contains(&instruction))
        continue;
      LLVMMemoryAnalysisResult error;
      auto descriptor =
          analyzeAccess(instruction, loop, dataLayout, scalarEvolution, addressUnitBytes, error);
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

      bool usedConservativeOrder = false;
      if (!instructionBefore(*lhs->instruction, *rhs->instruction, dominatorTree, loop)) {
        if (instructionBefore(*rhs->instruction, *lhs->instruction, dominatorTree, loop))
          std::swap(lhs, rhs);
        else if (blockOrder &&
                 blockOrder->at(lhs->instruction->getParent()) <
                     blockOrder->at(rhs->instruction->getParent()))
          usedConservativeOrder = true;
        else
          return fail(LLVMMemoryAnalysisStatus::UnsupportedPathSensitiveOrder,
                      "MayAlias accesses have no reducible CFG order for conservative ordering");
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

      const auto conservativeMode = usedConservativeOrder ||
                                             lhs->addressMode == LLVMAddressMode::Dynamic ||
                                             rhs->addressMode == LLVMAddressMode::Dynamic
                                         ? LLVMMemoryDependenceMode::DynamicConservative
                                         : LLVMMemoryDependenceMode::Conservative;
      addDependence(result, seen, lhs->id, rhs->id, dependenceKind(*lhs, *rhs), 0, conservativeMode,
                    usedConservativeOrder ? "conservative_total_order within one iteration"
                                          : "MayAlias program order within one iteration");
      addDependence(result, seen, rhs->id, lhs->id, dependenceKind(*rhs, *lhs), 1, conservativeMode,
                    usedConservativeOrder ? "conservative_total_order across iterations"
                                          : "MayAlias reverse order across iterations");
    }
  }
  return result;
}

} // namespace cgra::frontend::llvm_frontend
