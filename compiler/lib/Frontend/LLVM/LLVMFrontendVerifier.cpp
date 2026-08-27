// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"

#include "cgra/IR/DFGVerifier.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallVector.h>
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
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace cgra::frontend::llvm_frontend {
namespace {

using Json = nlohmann::json;

struct Selection {
  llvm::Function* function = nullptr;
  llvm::Loop* loop = nullptr;
  llvm::BasicBlock* block = nullptr;
  llvm::BasicBlock* exit = nullptr;
  llvm::BranchInst* branch = nullptr;
  std::unique_ptr<llvm::DominatorTree> dominatorTree;
  std::unique_ptr<llvm::LoopInfo> loopInfo;
};

std::string blockName(const llvm::Function& function, const llvm::BasicBlock& block) {
  if (block.hasName())
    return block.getName().str();
  std::uint32_t ordinal = 0;
  for (const auto& candidate : function) {
    if (&candidate == &block)
      break;
    ++ordinal;
  }
  return "bb." + std::to_string(ordinal);
}

bool ignored(const llvm::Instruction& instruction) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(instruction))
    return true;
  if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
    return intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
           intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end;
  return false;
}

std::optional<ir::Opcode> opcode(const llvm::Instruction& instruction) {
  switch (instruction.getOpcode()) {
  case llvm::Instruction::Add:
    return ir::Opcode::Add;
  case llvm::Instruction::Sub:
    return ir::Opcode::Sub;
  case llvm::Instruction::Mul:
    return ir::Opcode::Mul;
  case llvm::Instruction::And:
    return ir::Opcode::And;
  case llvm::Instruction::Or:
    return ir::Opcode::Or;
  case llvm::Instruction::Xor:
    return ir::Opcode::Xor;
  case llvm::Instruction::Shl:
    return ir::Opcode::Shl;
  case llvm::Instruction::LShr:
    return ir::Opcode::LShr;
  case llvm::Instruction::AShr:
    return ir::Opcode::AShr;
  default:
    return std::nullopt;
  }
}

std::optional<ir::ValueType> valueType(const llvm::Value& value) {
  const auto* integer = llvm::dyn_cast<llvm::IntegerType>(value.getType());
  if (!integer || integer->getBitWidth() == 0 || integer->getBitWidth() > 64)
    return std::nullopt;
  if (integer->getBitWidth() == 1)
    return ir::ValueType::predicate();
  return ir::ValueType::integer(static_cast<std::uint16_t>(integer->getBitWidth()));
}

std::optional<ir::ICmpPredicate> icmpPredicate(const llvm::ICmpInst& instruction) {
  switch (instruction.getPredicate()) {
  case llvm::CmpInst::ICMP_EQ:
    return ir::ICmpPredicate::EQ;
  case llvm::CmpInst::ICMP_NE:
    return ir::ICmpPredicate::NE;
  case llvm::CmpInst::ICMP_ULT:
    return ir::ICmpPredicate::ULT;
  case llvm::CmpInst::ICMP_ULE:
    return ir::ICmpPredicate::ULE;
  case llvm::CmpInst::ICMP_UGT:
    return ir::ICmpPredicate::UGT;
  case llvm::CmpInst::ICMP_UGE:
    return ir::ICmpPredicate::UGE;
  case llvm::CmpInst::ICMP_SLT:
    return ir::ICmpPredicate::SLT;
  case llvm::CmpInst::ICMP_SLE:
    return ir::ICmpPredicate::SLE;
  case llvm::CmpInst::ICMP_SGT:
    return ir::ICmpPredicate::SGT;
  case llvm::CmpInst::ICMP_SGE:
    return ir::ICmpPredicate::SGE;
  default:
    return std::nullopt;
  }
}

std::optional<ir::ICmpPredicate> complementPredicate(ir::ICmpPredicate predicate) {
  switch (predicate) {
  case ir::ICmpPredicate::EQ:
    return ir::ICmpPredicate::NE;
  case ir::ICmpPredicate::NE:
    return ir::ICmpPredicate::EQ;
  case ir::ICmpPredicate::ULT:
    return ir::ICmpPredicate::ULE;
  case ir::ICmpPredicate::ULE:
    return ir::ICmpPredicate::ULT;
  case ir::ICmpPredicate::UGT:
    return ir::ICmpPredicate::ULE;
  case ir::ICmpPredicate::UGE:
    return ir::ICmpPredicate::ULT;
  case ir::ICmpPredicate::SLT:
    return ir::ICmpPredicate::SLE;
  case ir::ICmpPredicate::SLE:
    return ir::ICmpPredicate::SLT;
  case ir::ICmpPredicate::SGT:
    return ir::ICmpPredicate::SLE;
  case ir::ICmpPredicate::SGE:
    return ir::ICmpPredicate::SLT;
  }
  return std::nullopt;
}

bool complementSwapsOperands(ir::ICmpPredicate predicate) {
  return predicate == ir::ICmpPredicate::ULT || predicate == ir::ICmpPredicate::ULE ||
         predicate == ir::ICmpPredicate::SLT || predicate == ir::ICmpPredicate::SLE;
}

std::vector<llvm::Loop*> innermost(llvm::LoopInfo& info) {
  std::vector<llvm::Loop*> result;
  std::function<void(llvm::Loop*)> visit = [&](llvm::Loop* loop) {
    if (loop->isInnermost()) {
      result.push_back(loop);
      return;
    }
    for (auto* child : *loop)
      visit(child);
  };
  for (auto* loop : info)
    visit(loop);
  return result;
}

std::optional<Selection> select(const llvm::Module& module, const LLVMFrontendOptions& options,
                                LLVMFrontendVerificationReport& report) {
  auto& mutableModule = const_cast<llvm::Module&>(module);
  auto* function = mutableModule.getFunction(options.functionName);
  if (!function) {
    report.add("LLVM_FRONTEND_FUNCTION_NOT_FOUND", "function not found: " + options.functionName);
    return std::nullopt;
  }
  auto dominatorTree = std::make_unique<llvm::DominatorTree>(*function);
  auto loopInfo = std::make_unique<llvm::LoopInfo>(*dominatorTree);
  auto loops = innermost(*loopInfo);
  llvm::Loop* selected = nullptr;
  if (options.loopHeader) {
    for (auto* loop : loops)
      if (blockName(*function, *loop->getHeader()) == *options.loopHeader)
        selected = loop;
  } else if (loops.size() == 1) {
    selected = loops.front();
  }
  if (!selected) {
    report.add(options.loopHeader ? "LLVM_FRONTEND_NO_INNERMOST_LOOP"
                                  : "LLVM_FRONTEND_AMBIGUOUS_LOOP",
               "verifier could not identify the selected innermost loop");
    return std::nullopt;
  }
  Selection selection;
  selection.function = function;
  selection.loop = selected;
  selection.block = selected->getHeader();
  selection.exit = selected->getExitBlock();
  selection.branch = llvm::dyn_cast<llvm::BranchInst>(selected->getHeader()->getTerminator());
  selection.dominatorTree = std::move(dominatorTree);
  selection.loopInfo = std::move(loopInfo);
  return selection;
}

std::unordered_set<const llvm::Instruction*> controlSlice(const Selection& selection) {
  std::unordered_set<const llvm::Instruction*> result;
  std::vector<const llvm::Value*> work;
  for (const auto* block : selection.loop->getBlocks()) {
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isConditional())
      continue;
    bool exitsLoop = false;
    for (unsigned successor = 0; successor < branch->getNumSuccessors(); ++successor)
      exitsLoop |= !selection.loop->contains(branch->getSuccessor(successor));
    if (exitsLoop)
      work.push_back(branch->getCondition());
  }
  while (!work.empty()) {
    const auto* value = work.back();
    work.pop_back();
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || !selection.loop->contains(instruction) ||
        !result.insert(instruction).second)
      continue;
    for (const auto& operand : instruction->operands())
      work.push_back(operand.get());
  }
  return result;
}

void removeRecurrenceProducerClosure(const Selection& selection,
                                     std::unordered_set<const llvm::Instruction*>& slice) {
  std::vector<const llvm::Instruction*> work;
  const auto* preheader = selection.loop->getLoopPreheader();
  const auto* latch = selection.loop->getLoopLatch();
  if (preheader && latch) {
    for (const auto& instruction : *selection.block) {
      const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
      if (!phi || phi->getNumIncomingValues() != 2 || phi->getBasicBlockIndex(preheader) < 0)
        continue;
      const int latchIndex = phi->getBasicBlockIndex(latch);
      if (latchIndex < 0)
        continue;
      const auto* backedge = llvm::dyn_cast<llvm::Instruction>(
          phi->getIncomingValue(static_cast<unsigned>(latchIndex)));
      if (!backedge || !selection.loop->contains(backedge) || llvm::isa<llvm::PHINode>(backedge) ||
          !opcode(*backedge))
        continue;
      const bool hasDataUse = std::ranges::any_of(phi->users(), [&](const llvm::User* user) {
        const auto* use = llvm::dyn_cast<llvm::Instruction>(user);
        return use && selection.loop->contains(use) && !slice.contains(use) &&
               !use->isTerminator() && !ignored(*use);
      });
      if (hasDataUse)
        work.push_back(backedge);
    }
  }

  std::unordered_set<const llvm::Instruction*> visited;
  while (!work.empty()) {
    const auto* instruction = work.back();
    work.pop_back();
    if (const auto [_, inserted] = visited.insert(instruction); !inserted)
      continue;
    slice.erase(instruction);
    for (const auto& operand : instruction->operands()) {
      const auto* dependency = llvm::dyn_cast<llvm::Instruction>(operand.get());
      if (!dependency || !selection.loop->contains(dependency) ||
          llvm::isa<llvm::PHINode>(dependency) || !opcode(*dependency))
        continue;
      work.push_back(dependency);
    }
  }
}

bool trivialLCSSA(const llvm::PHINode& phi, const llvm::Value& source, const Selection& selection) {
  if (phi.getParent() != selection.exit)
    return false;
  unsigned sourceCount = 0;
  for (unsigned index = 0; index < phi.getNumIncomingValues(); ++index) {
    const auto* incoming = phi.getIncomingValue(index);
    if (incoming == &source)
      ++sourceCount;
    else if (!llvm::isa<llvm::UndefValue>(incoming))
      return false;
  }
  return sourceCount == 1;
}

bool hasOutsideUse(const llvm::Value& value, const Selection& selection) {
  for (const auto* user : value.users()) {
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
    if (instruction && selection.loop->contains(instruction))
      continue;
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
      if (!trivialLCSSA(*phi, value, selection))
        return false;
      for (const auto* phiUser : phi->users()) {
        const auto* phiInstruction = llvm::dyn_cast<llvm::Instruction>(phiUser);
        if (!phiInstruction || !selection.loop->contains(phiInstruction))
          return true;
      }
      continue;
    }
    if (instruction && ignored(*instruction))
      continue;
    return true;
  }
  return false;
}

const LLVMFrontendNodeProvenance* nodeProvenance(const LLVMFrontendResult& result, ir::NodeId id) {
  for (const auto& item : result.provenance.nodes)
    if (item.node == id)
      return &item;
  return nullptr;
}

const LLVMRecurrenceProvenance* recurrenceForEdge(const LLVMFrontendResult& result,
                                                  const llvm::Instruction* source,
                                                  const llvm::Value* phi) {
  for (const auto& recurrence : result.provenance.recurrences)
    if (recurrence.backedge == source && recurrence.phiValue == phi)
      return &recurrence;
  return nullptr;
}

bool boundaryMatches(const LLVMFrontendResult& result, const ir::DFG& dfg,
                     const LLVMRecurrenceProvenance& recurrence,
                     const ir::RecurrenceBoundary& boundary) {
  if (boundary.values.size() != 1 || boundary.values.front().iterationOffset != 0)
    return false;
  const auto& value = boundary.values.front().value;
  if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(recurrence.initial)) {
    if (!std::holds_alternative<ir::ConstantRef>(value))
      return false;
    const auto id = std::get<ir::ConstantRef>(value).value;
    if (!dfg.containsConstant(id))
      return false;
    const auto type = valueType(*constant);
    return type && dfg.constant(id).type == *type &&
           dfg.constant(id).bits == constant->getValue().getZExtValue();
  }
  if (!std::holds_alternative<ir::ExternalValueRef>(value))
    return false;
  const auto id = std::get<ir::ExternalValueRef>(value).value;
  for (const auto& external : result.provenance.externals)
    if (external.external == id && external.value == recurrence.initial)
      return true;
  return false;
}

const ir::Edge* findProviderEdge(const ir::DFG& dfg, ir::NodeId destination, std::uint32_t operand,
                                 ir::Edge::Kind kind) {
  for (const auto edgeId : dfg.incoming(destination)) {
    const auto& edge = dfg.edge(edgeId);
    if (edge.kind() != kind)
      continue;
    const auto edgeOperand = kind == ir::Edge::Kind::Predicate
                                 ? std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand
                                 : std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
    if (edgeOperand == operand)
      return &edge;
  }
  return nullptr;
}

bool providerMatches(const LLVMFrontendResult& result, const llvm::Value* value,
                     ir::NodeId destination, std::uint32_t operand) {
  if (!value)
    return false;
  if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    for (const auto& recurrence : result.provenance.recurrences) {
      if (recurrence.phiValue != phi)
        continue;
      const auto* edge = findProviderEdge(*result.dfg, destination, operand, ir::Edge::Kind::Data);
      if (!edge || edge->distance != 1)
        return false;
      const auto* source = nodeProvenance(result, edge->src);
      if (!source || source->instruction != recurrence.backedge)
        return false;
      const auto* data = std::get_if<ir::DataEdgeInfo>(&edge->info);
      return data && data->boundary &&
             boundaryMatches(result, *result.dfg, recurrence, *data->boundary);
    }
    return false;
  }
  if (const auto* edge =
          findProviderEdge(*result.dfg, destination, operand, ir::Edge::Kind::Data)) {
    const auto* source = nodeProvenance(result, edge->src);
    if (source && source->instruction == value)
      return true;
  }
  for (const auto& binding : result.dfg->externalBindings()) {
    if (binding.node != destination || binding.operand != operand)
      continue;
    if (const auto* external = std::get_if<ir::ExternalValueRef>(&binding.source)) {
      for (const auto& provenance : result.provenance.externals)
        if (provenance.external == external->value && provenance.value == value)
          return true;
    } else if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      const auto* reference = std::get_if<ir::ConstantRef>(&binding.source);
      if (!reference)
        return false;
      const auto id = reference->value;
      if (result.dfg->containsConstant(id) &&
          result.dfg->constant(id).bits == constant->getValue().getZExtValue() &&
          valueType(*constant) && result.dfg->constant(id).type == *valueType(*constant))
        return true;
    }
  }
  return false;
}

llvm::BasicBlock* unconditionalSuccessor(llvm::BasicBlock* block) {
  const auto* branch = block ? llvm::dyn_cast<llvm::BranchInst>(block->getTerminator()) : nullptr;
  if (!branch || !branch->isUnconditional())
    return nullptr;
  return branch->getSuccessor(0);
}

llvm::BasicBlock* branchMerge(const llvm::BranchInst& branch) {
  auto* trueBlock = branch.getSuccessor(0);
  auto* falseBlock = branch.getSuccessor(1);
  auto* trueSuccessor = unconditionalSuccessor(trueBlock);
  auto* falseSuccessor = unconditionalSuccessor(falseBlock);
  if (trueSuccessor && trueSuccessor == falseBlock)
    return falseBlock;
  if (falseSuccessor && falseSuccessor == trueBlock)
    return trueBlock;
  if (trueSuccessor && trueSuccessor == falseSuccessor)
    return trueSuccessor;
  return nullptr;
}

std::uint32_t correspondingLLVMOperand(const LLVMFrontendResult& result,
                                       const llvm::Instruction& instruction,
                                       std::uint32_t genericOperand) {
  if (llvm::isa<llvm::StoreInst>(instruction) && genericOperand < 2)
    return 1U - genericOperand;
  const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction);
  if (!compare || genericOperand >= compare->getNumOperands())
    return genericOperand;
  const auto predicate = icmpPredicate(*compare);
  if (!predicate || !complementSwapsOperands(*predicate))
    return genericOperand;
  for (const auto& region : result.provenance.ifConversions)
    if (region.conditionValue == compare && region.predicateComplemented)
      return 1U - genericOperand;
  return genericOperand;
}

void verifyIfRegionStructure(const Selection& selection, const LLVMFrontendResult& result,
                             LLVMFrontendVerificationReport& report) {
  std::vector<const llvm::BranchInst*> internalBranches;
  for (const auto* block : selection.loop->getBlocks()) {
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (branch && branch->isConditional() && selection.loop->contains(branch->getSuccessor(0)) &&
        selection.loop->contains(branch->getSuccessor(1)))
      internalBranches.push_back(branch);
  }

  std::size_t describedBranches = 0;
  for (const auto& region : result.provenance.ifConversions) {
    if (!region.branch)
      continue;
    ++describedBranches;
    if (internalBranches.size() != 1 || region.branch != internalBranches.front()) {
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                 "if-conversion plan does not identify the unique internal branch");
      continue;
    }
    const auto* branch = internalBranches.front();
    if (region.conditionValue != branch->getCondition() ||
        region.conditionBlock != blockName(*selection.function, *branch->getParent())) {
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                 "if-conversion condition does not match the LLVM branch");
    }
    auto* trueBlock = branch->getSuccessor(region.predicateComplemented ? 1 : 0);
    auto* falseBlock = branch->getSuccessor(region.predicateComplemented ? 0 : 1);
    if (region.trueBlock != blockName(*selection.function, *trueBlock) ||
        region.falseBlock != blockName(*selection.function, *falseBlock)) {
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                 "normalized branch arms do not match LLVM CFG successors");
    }

    auto* merge = branchMerge(*branch);
    if (!merge || region.mergeBlock != blockName(*selection.function, *merge))
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                 "if-conversion merge does not match the LLVM diamond or triangle");
  }
  if (describedBranches != internalBranches.size())
    report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
               "internal LLVM branch coverage does not match if-conversion plans");
}

void verifyLinearRegionStructure(const Selection& selection, const LLVMFrontendResult& result,
                                 LLVMFrontendVerificationReport& report) {
  if (selection.loop->getBlocks().size() <= 1) {
    if (result.provenance.linearLoop)
      report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
                 "single-block loop has spurious linear-loop provenance");
    return;
  }

  const bool hasBranchPlan = std::ranges::any_of(
      result.provenance.ifConversions, [](const auto& region) { return region.branch != nullptr; });
  if (hasBranchPlan) {
    if (result.provenance.linearLoop)
      report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
                 "if-converted loop has spurious linear-loop provenance");
    return;
  }

  auto* preheader = selection.loop->getLoopPreheader();
  auto* header = selection.loop->getHeader();
  auto* latch = selection.loop->getLoopLatch();
  llvm::SmallVector<llvm::BasicBlock*, 4> exitingBlocks;
  llvm::SmallVector<llvm::BasicBlock*, 4> exitBlocks;
  selection.loop->getExitingBlocks(exitingBlocks);
  selection.loop->getExitBlocks(exitBlocks);
  if (!preheader || !latch || exitingBlocks.size() != 1 || exitBlocks.size() != 1) {
    report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
               "multi-block result does not have the unique preheader/latch/exit linear shape");
    return;
  }

  auto* exiting = exitingBlocks.front();
  auto* exit = exitBlocks.front();
  const llvm::BranchInst* termination = nullptr;
  std::unordered_map<llvm::BasicBlock*, llvm::BasicBlock*> nextBlock;
  std::unordered_map<llvm::BasicBlock*, unsigned> forwardPredecessors;
  bool invalidShape = exiting != header && exiting != latch;
  for (auto* block : selection.loop->getBlocks()) {
    for (const auto& instruction : *block)
      invalidShape |= llvm::isa<llvm::PHINode>(instruction) && block != header;
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch) {
      invalidShape = true;
      continue;
    }
    if (branch->isConditional()) {
      const bool firstInside = selection.loop->contains(branch->getSuccessor(0));
      const bool secondInside = selection.loop->contains(branch->getSuccessor(1));
      const bool exitsSelectedLoop =
          firstInside != secondInside &&
          (firstInside ? branch->getSuccessor(1) : branch->getSuccessor(0)) == exit;
      if (termination || branch->getParent() != exiting || !exitsSelectedLoop)
        invalidShape = true;
      else
        termination = branch;
    } else if (!selection.loop->contains(branch->getSuccessor(0))) {
      invalidShape = true;
    }

    for (auto* successor : llvm::successors(block)) {
      if (!selection.loop->contains(successor) || (block == latch && successor == header))
        continue;
      if (nextBlock.contains(block))
        invalidShape = true;
      else
        nextBlock.emplace(block, successor);
      ++forwardPredecessors[successor];
    }
  }
  for (auto* block : selection.loop->getBlocks()) {
    const auto expectedPredecessors = block == header ? 0U : 1U;
    const auto expectedSuccessors = block == latch ? 0U : 1U;
    invalidShape |= forwardPredecessors[block] != expectedPredecessors;
    invalidShape |= static_cast<unsigned>(nextBlock.contains(block)) != expectedSuccessors;
  }

  std::vector<llvm::BasicBlock*> orderedBlocks;
  std::unordered_set<llvm::BasicBlock*> visited;
  auto* block = header;
  while (block && visited.insert(block).second) {
    orderedBlocks.push_back(block);
    if (block == latch)
      break;
    const auto next = nextBlock.find(block);
    block = next == nextBlock.end() ? nullptr : next->second;
  }
  invalidShape |=
      !termination || block != latch || orderedBlocks.size() != selection.loop->getBlocks().size();
  if (invalidShape) {
    report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
               "multi-block result is not an independently reconstructed linear CFG path");
    return;
  }
  if (!result.provenance.linearLoop) {
    report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
               "linear-loop result has no structural provenance");
    return;
  }

  const auto& actual = *result.provenance.linearLoop;
  std::vector<std::string> ordered;
  for (const auto* block : orderedBlocks)
    ordered.push_back(blockName(*selection.function, *block));
  if (actual.header != blockName(*selection.function, *header) ||
      actual.preheader != blockName(*selection.function, *preheader) ||
      actual.latch != blockName(*selection.function, *latch) ||
      actual.exiting != blockName(*selection.function, *exiting) ||
      actual.exit != blockName(*selection.function, *exit) ||
      actual.terminationBlock != blockName(*selection.function, *termination->getParent()) ||
      actual.orderedBlocks != ordered) {
    report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
               "linear-loop provenance does not match independently reconstructed CFG order");
  }
  if (!result.metadata || result.metadata->loopShape != "linear_multiblock" ||
      result.metadata->loopBlockCount != orderedBlocks.size())
    report.add("LLVM_FRONTEND_LINEAR_LOOP_VERIFY_FAILED",
               "linear-loop metadata does not match the selected LLVM loop");
}

void verifyIfDataflow(const Selection& selection, const LLVMFrontendResult& result,
                      LLVMFrontendVerificationReport& report) {
  auto slice = controlSlice(selection);
  removeRecurrenceProducerClosure(selection, slice);
  std::unordered_set<const llvm::Instruction*> mappedInstructions;
  std::unordered_set<ir::NodeId> plannedSelects;
  std::unordered_set<ir::NodeId> plannedStores;
  std::unordered_set<ir::NodeId> plannedMemoryNodes;
  for (const auto& access : result.provenance.memoryAccesses)
    plannedMemoryNodes.insert(access.memoryNode);
  for (const auto& region : result.provenance.ifConversions) {
    for (const auto& select : region.selects)
      if (!plannedSelects.insert(select.node).second)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "Select is duplicated across if-conversion plans");
    for (const auto store : region.predicatedStores)
      if (!plannedStores.insert(store).second)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "Store is duplicated across if-conversion plans");
  }

  std::unordered_set<const llvm::PHINode*> mergePhis;
  for (const auto* block : selection.loop->getBlocks()) {
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isConditional() || !selection.loop->contains(branch->getSuccessor(0)) ||
        !selection.loop->contains(branch->getSuccessor(1)))
      continue;
    const auto* merge = branchMerge(*branch);
    if (!merge)
      continue;
    for (const auto& instruction : *merge) {
      const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
      if (!phi)
        break;
      mergePhis.insert(phi);
    }
  }

  for (const auto* phi : mergePhis) {
    std::vector<ir::NodeId> matchingNodes;
    for (const auto& provenance : result.provenance.nodes)
      if (provenance.instruction == phi && result.dfg->containsNode(provenance.node) &&
          result.dfg->node(provenance.node).opcode == ir::Opcode::Select)
        matchingNodes.push_back(provenance.node);
    std::vector<ir::NodeId> plannedNodes;
    for (const auto& region : result.provenance.ifConversions)
      for (const auto& select : region.selects)
        if (select.phiValue == phi)
          plannedNodes.push_back(select.node);
    if (matchingNodes.size() != 1 || plannedNodes.size() != 1 ||
        (matchingNodes.size() == 1 && plannedNodes.size() == 1 &&
         matchingNodes.front() != plannedNodes.front()))
      report.add("LLVM_FRONTEND_SELECT_SEMANTICS_MISMATCH",
                 "LLVM control-merge PHI must correspond to exactly one planned Generic Select");
  }

  for (const auto& node : result.dfg->nodes()) {
    const auto* provenance = nodeProvenance(result, node.id);
    if (!provenance || !provenance->instruction) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_MISSING",
                 "Generic node has no LLVM instruction provenance");
      continue;
    }
    const auto* instruction = provenance->instruction;
    if (!selection.loop->contains(instruction) || slice.contains(instruction) ||
        ignored(*instruction)) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_INVALID",
                 "node provenance is outside the selected data path");
      continue;
    }
    std::uint32_t instructionOrdinal = 0;
    for (const auto& candidate : *instruction->getParent()) {
      if (&candidate == instruction)
        break;
      ++instructionOrdinal;
    }
    if (provenance->function != selection.function->getName().str() ||
        provenance->basicBlock != blockName(*selection.function, *instruction->getParent()) ||
        provenance->instructionOrdinal != instructionOrdinal) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_INVALID",
                 "node function, block, or instruction ordinal provenance is inconsistent");
    }
    mappedInstructions.insert(instruction);
    if (const auto expected = opcode(*instruction)) {
      const auto type = valueType(*instruction);
      if (!type || node.opcode != *expected || node.resultType != *type)
        report.add("LLVM_FRONTEND_NODE_SEMANTICS_MISMATCH",
                   "Generic arithmetic node does not match its LLVM instruction");
    } else if (llvm::isa<llvm::SelectInst>(instruction) || llvm::isa<llvm::PHINode>(instruction)) {
      if (node.opcode != ir::Opcode::Select || !plannedSelects.contains(node.id))
        report.add("LLVM_FRONTEND_SELECT_SEMANTICS_MISMATCH",
                   "Generic Select is not covered by exactly one LLVM merge/select plan");
    } else if (llvm::isa<llvm::StoreInst>(instruction)) {
      if (node.opcode != ir::Opcode::Store || !plannedMemoryNodes.contains(node.id) ||
          (node.operandTypes.size() == 3 && !plannedStores.contains(node.id)))
        report.add("LLVM_FRONTEND_STORE_SEMANTICS_MISMATCH",
                   "Generic Store is not covered by memory/predication provenance");
    } else if (llvm::isa<llvm::LoadInst>(instruction)) {
      if (node.opcode != ir::Opcode::Load || node.resultType != ir::ValueType::i32() ||
          !node.memoryInfo || node.memoryInfo->accessWidthBits != 32 ||
          !plannedMemoryNodes.contains(node.id))
        report.add("LLVM_FRONTEND_MEMORY_NODE_SEMANTICS_MISMATCH",
                   "Generic Load does not match its LLVM memory access");
    } else if (llvm::isa<llvm::GetElementPtrInst>(instruction)) {
      const auto expectedOpcode =
          provenance->opcode == "GEP_SCALE" ? ir::Opcode::Mul : ir::Opcode::Add;
      if (node.opcode != expectedOpcode || node.resultType != ir::ValueType::i32())
        report.add("LLVM_FRONTEND_ADDRESS_SEMANTICS_MISMATCH",
                   "LLVM GEP must lower to Generic word-address arithmetic");
    } else if (!llvm::isa<llvm::ICmpInst>(instruction)) {
      report.add("LLVM_FRONTEND_NODE_SEMANTICS_MISMATCH",
                 "Generic node provenance names an unsupported LLVM instruction");
    }
    if (opcode(*instruction) || llvm::isa<llvm::ICmpInst>(instruction)) {
      for (std::uint32_t operand = 0; operand < node.operandTypes.size(); ++operand) {
        const auto llvmOperand = correspondingLLVMOperand(result, *instruction, operand);
        if (llvmOperand >= instruction->getNumOperands() ||
            !providerMatches(result, instruction->getOperand(llvmOperand), node.id, operand))
          report.add("LLVM_FRONTEND_EDGE_PROVENANCE_INVALID",
                     "Generic operand provider does not match its LLVM SSA operand");
      }
    }
  }

  for (const auto& edge : result.dfg->edges()) {
    if (edge.kind() != ir::Edge::Kind::Data)
      continue;
    const auto* source = nodeProvenance(result, edge.src);
    const auto* destination = nodeProvenance(result, edge.dst);
    if (!source || !destination || !source->instruction || !destination->instruction) {
      report.add("LLVM_FRONTEND_EDGE_PROVENANCE_MISSING", "edge endpoint provenance is missing");
      continue;
    }
    const auto info = std::get<ir::DataEdgeInfo>(edge.info);
    if (llvm::isa<llvm::GetElementPtrInst>(destination->instruction))
      continue;
    const auto llvmOperand =
        correspondingLLVMOperand(result, *destination->instruction, info.dstOperand);
    if (edge.distance == 0) {
      if (llvm::isa<llvm::PHINode>(destination->instruction) ||
          llvm::isa<llvm::SelectInst>(destination->instruction) ||
          llvm::isa<llvm::StoreInst>(destination->instruction))
        continue;
      if (info.boundary || llvmOperand >= destination->instruction->getNumOperands() ||
          destination->instruction->getOperand(llvmOperand) != source->instruction)
        report.add("LLVM_FRONTEND_EDGE_PROVENANCE_INVALID",
                   "Generic data edge does not match the LLVM SSA def-use operand");
      continue;
    }
    const auto* recurrence =
        llvmOperand < destination->instruction->getNumOperands()
            ? recurrenceForEdge(result, source->instruction,
                                destination->instruction->getOperand(llvmOperand))
            : nullptr;
    if (edge.distance != 1 || !info.boundary || !recurrence ||
        !boundaryMatches(result, *result.dfg, *recurrence, *info.boundary))
      report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                 "distance-one edge does not match a canonical LLVM PHI use and boundary");
  }

  for (const auto& recurrence : result.provenance.recurrences) {
    if (!recurrence.phiValue || !recurrence.backedge || recurrence.distance != 1) {
      report.add("LLVM_FRONTEND_RECURRENCE_BOUNDARY_VERIFY_FAILED",
                 "recurrence descriptor is incomplete");
      continue;
    }
    for (const auto& use : recurrence.uses) {
      if (!result.dfg->containsEdge(use.edge)) {
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence descriptor references a missing Generic edge");
        continue;
      }
      const auto& edge = result.dfg->edge(use.edge);
      const auto* source = nodeProvenance(result, edge.src);
      const auto* destination = nodeProvenance(result, edge.dst);
      const auto* info = std::get_if<ir::DataEdgeInfo>(&edge.info);
      const auto llvmOperand =
          info && destination && destination->instruction
              ? correspondingLLVMOperand(result, *destination->instruction, info->dstOperand)
              : 0;
      const bool gepUsesPhi =
          destination && llvm::isa<llvm::GetElementPtrInst>(destination->instruction) &&
          std::ranges::any_of(destination->instruction->operands(), [&](const auto& operand) {
            return operand.get() == recurrence.phiValue;
          });
      if (edge.kind() != ir::Edge::Kind::Data || edge.distance != 1 ||
          edge.dst != use.destination || !source || !destination || !destination->instruction ||
          source->instruction != recurrence.backedge || !info || info->dstOperand != use.operand ||
          (!gepUsesPhi &&
           (llvmOperand >= destination->instruction->getNumOperands() ||
            destination->instruction->getOperand(llvmOperand) != recurrence.phiValue)) ||
          !info->boundary || !boundaryMatches(result, *result.dfg, recurrence, *info->boundary))
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence descriptor edge identity is inconsistent");
    }

    for (const auto* user : recurrence.phiValue->users()) {
      const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (!instruction || !selection.loop->contains(instruction) || slice.contains(instruction) ||
          instruction->isTerminator() || ignored(*instruction))
        continue;
      if (llvm::isa<llvm::GetElementPtrInst>(instruction)) {
        bool matched = false;
        for (const auto& destination : result.provenance.nodes) {
          if (destination.instruction != instruction || !result.dfg->containsNode(destination.node))
            continue;
          for (std::uint32_t operand = 0;
               operand < result.dfg->node(destination.node).operandTypes.size(); ++operand) {
            const auto* edge =
                findProviderEdge(*result.dfg, destination.node, operand, ir::Edge::Kind::Data);
            const auto* info = edge ? std::get_if<ir::DataEdgeInfo>(&edge->info) : nullptr;
            const auto* source = edge ? nodeProvenance(result, edge->src) : nullptr;
            matched |= edge && edge->distance == 1 && source &&
                       source->instruction == recurrence.backedge && info && info->boundary &&
                       boundaryMatches(result, *result.dfg, recurrence, *info->boundary);
          }
        }
        if (!matched)
          report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                     "recurrence PHI GEP use is missing its Generic address edge");
        continue;
      }
      const auto destination = std::ranges::find_if(result.provenance.nodes, [&](const auto& item) {
        return item.instruction == instruction;
      });
      if (destination == result.provenance.nodes.end()) {
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence PHI data use has no Generic destination node");
        continue;
      }
      for (unsigned operand = 0; operand < instruction->getNumOperands(); ++operand) {
        if (instruction->getOperand(operand) != recurrence.phiValue)
          continue;
        const auto genericOperand = correspondingLLVMOperand(result, *instruction, operand);
        const auto* edge =
            findProviderEdge(*result.dfg, destination->node, genericOperand, ir::Edge::Kind::Data);
        const auto* info = edge ? std::get_if<ir::DataEdgeInfo>(&edge->info) : nullptr;
        const auto* source = edge ? nodeProvenance(result, edge->src) : nullptr;
        if (!edge || edge->distance != 1 || !source || source->instruction != recurrence.backedge ||
            !info || !info->boundary ||
            !boundaryMatches(result, *result.dfg, recurrence, *info->boundary))
          report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                     "recurrence PHI data use is missing its Generic edge");
      }
    }
  }

  std::unordered_set<const llvm::Value*> externalValues;
  for (const auto& external : result.dfg->externalValues()) {
    const auto provenance =
        std::ranges::find_if(result.provenance.externals,
                             [&](const auto& item) { return item.external == external.id; });
    if (provenance == result.provenance.externals.end() || !provenance->value) {
      report.add("LLVM_FRONTEND_EXTERNAL_PROVENANCE_MISSING",
                 "ExternalValue has no LLVM value provenance");
      continue;
    }
    const auto* definingInstruction = llvm::dyn_cast<llvm::Instruction>(provenance->value);
    const bool supportedPointerBridge = provenance->value->getType()->isPointerTy();
    const auto expectedType = supportedPointerBridge ? std::optional{ir::ValueType::i32()}
                                                     : valueType(*provenance->value);
    bool referenced = false;
    for (const auto& binding : result.dfg->externalBindings())
      if (const auto* reference = std::get_if<ir::ExternalValueRef>(&binding.source))
        referenced |= reference->value == external.id;
    for (const auto& edge : result.dfg->edges()) {
      const auto* data = std::get_if<ir::DataEdgeInfo>(&edge.info);
      if (!data || !data->boundary)
        continue;
      for (const auto& boundary : data->boundary->values)
        if (const auto* reference = std::get_if<ir::ExternalValueRef>(&boundary.value))
          referenced |= reference->value == external.id;
    }
    if ((definingInstruction && selection.loop->contains(definingInstruction)) ||
        llvm::isa<llvm::ConstantInt>(provenance->value) || !expectedType ||
        external.type != *expectedType || !referenced ||
        !externalValues.insert(provenance->value).second)
      report.add("LLVM_FRONTEND_EXTERNAL_PROVENANCE_INVALID",
                 "ExternalValue provenance is not a unique loop-external scalar/base");
  }

  for (const auto& liveOut : result.dfg->liveOuts()) {
    const auto provenance = std::ranges::find_if(
        result.provenance.liveOuts, [&](const auto& item) { return item.liveOut == liveOut.id; });
    const auto* source = nodeProvenance(result, liveOut.source);
    if (provenance == result.provenance.liveOuts.end() || !provenance->value || !source ||
        source->instruction != provenance->value || !hasOutsideUse(*provenance->value, selection))
      report.add("LLVM_FRONTEND_LIVEOUT_PROVENANCE_INVALID",
                 "LiveOut does not identify an in-loop value with an outside use");
  }

  for (const auto& provenance : result.provenance.nodes) {
    if (!provenance.instruction || !result.dfg->containsNode(provenance.node) ||
        result.dfg->node(provenance.node).resultType == ir::ValueType::voidTy() ||
        !hasOutsideUse(*provenance.instruction, selection))
      continue;
    const auto count = std::ranges::count_if(result.dfg->liveOuts(), [&](const auto& liveOut) {
      return liveOut.source == provenance.node;
    });
    if (count != 1)
      report.add("LLVM_FRONTEND_LIVEOUT_PROVENANCE_INVALID",
                 "outside-used LLVM value must correspond to exactly one Generic LiveOut");
  }

  for (const auto* block : selection.loop->getBlocks())
    for (const auto& instruction : *block) {
      if (instruction.isTerminator() || ignored(instruction) ||
          llvm::isa<llvm::PHINode>(instruction) || slice.contains(&instruction))
        continue;
      if (!mappedInstructions.contains(&instruction))
        report.add("LLVM_FRONTEND_SILENT_INSTRUCTION_LOSS",
                   "semantic LLVM instruction is absent from Generic DFG provenance");
    }
}

enum class VerifiedMemoryAccessKind { Load, Store };
enum class VerifiedMemoryDependenceMode { ExactAffine, Conservative, DynamicConservative };

struct VerifiedMemoryAccess {
  std::uint32_t id = 0;
  VerifiedMemoryAccessKind kind = VerifiedMemoryAccessKind::Load;
  const llvm::Instruction* instruction = nullptr;
  const llvm::Value* address = nullptr;
  const llvm::Value* base = nullptr;
  const llvm::Value* dynamicIndex = nullptr;
  LLVMAddressMode addressMode = LLVMAddressMode::ExactAffine;
  std::string invariantExpression;
  std::int64_t constantOffsetBytes = 0;
  std::int64_t iterationStrideBytes = 0;
  std::int64_t dynamicScaleWords = 0;
  std::int64_t gepConstantOffsetWords = 0;
  std::int64_t constantOffsetWords = 0;
  std::int64_t iterationStrideWords = 0;
  std::uint32_t accessWidthBits = 0;
};

struct VerifiedMemoryDependence {
  std::uint32_t sourceAccess = 0;
  std::uint32_t destinationAccess = 0;
  ir::MemoryDepKind kind = ir::MemoryDepKind::RAW;
  std::uint32_t distance = 0;
  VerifiedMemoryDependenceMode mode = VerifiedMemoryDependenceMode::ExactAffine;
};

struct VerifiedMemoryExpectations {
  std::string error;
  std::vector<VerifiedMemoryAccess> accesses;
  std::vector<VerifiedMemoryDependence> dependences;

  bool ok() const noexcept { return error.empty(); }
};

struct VerifiedAffineValue {
  std::int64_t offset = 0;
  std::int64_t stride = 0;
  LLVMAddressMode mode = LLVMAddressMode::ExactAffine;
  std::string invariantExpression;
};

bool verifiedFitsAddressWord(std::int64_t value) {
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

const llvm::Value* verifiedPointerOperand(const llvm::Instruction& instruction) {
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
    return load->getPointerOperand();
  if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
    return store->getPointerOperand();
  return nullptr;
}

std::optional<std::int64_t> verifiedSignedConstant(const llvm::SCEV* expression) {
  const auto* constant = llvm::dyn_cast<llvm::SCEVConstant>(expression);
  if (!constant || constant->getAPInt().getMinSignedBits() > 64)
    return std::nullopt;
  return constant->getAPInt().getSExtValue();
}

std::optional<VerifiedAffineValue> verifiedAffineValue(const llvm::Value& value,
                                                       const llvm::Loop& loop,
                                                       llvm::ScalarEvolution& scalarEvolution) {
  const auto* expression = scalarEvolution.getSCEV(const_cast<llvm::Value*>(&value));
  if (const auto constant = verifiedSignedConstant(expression))
    return VerifiedAffineValue{*constant, 0, LLVMAddressMode::ExactAffine, {}};
  if (scalarEvolution.isLoopInvariant(expression, &loop)) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    expression->print(stream);
    stream.flush();
    return VerifiedAffineValue{0, 0, LLVMAddressMode::SymbolicAffine, std::move(text)};
  }
  const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(expression);
  if (recurrence && recurrence->getLoop() == &loop && recurrence->isAffine()) {
    const auto step = verifiedSignedConstant(recurrence->getStepRecurrence(scalarEvolution));
    if (!step)
      return VerifiedAffineValue{0, 0, LLVMAddressMode::Dynamic, {}};
    if (const auto start = verifiedSignedConstant(recurrence->getStart()))
      return VerifiedAffineValue{*start, *step, LLVMAddressMode::ExactAffine, {}};
    if (scalarEvolution.isLoopInvariant(recurrence->getStart(), &loop)) {
      std::string text;
      llvm::raw_string_ostream stream(text);
      recurrence->getStart()->print(stream);
      stream.flush();
      return VerifiedAffineValue{0, *step, LLVMAddressMode::SymbolicAffine, std::move(text)};
    }
  }
  return VerifiedAffineValue{0, 0, LLVMAddressMode::Dynamic, {}};
}

bool verifiedInstructionBefore(const llvm::Instruction& lhs, const llvm::Instruction& rhs,
                               const llvm::DominatorTree& dominatorTree) {
  if (lhs.getParent() == rhs.getParent())
    return lhs.comesBefore(&rhs);
  return dominatorTree.dominates(lhs.getParent(), rhs.getParent());
}

bool verifiedIsStore(const VerifiedMemoryAccess& access) {
  return access.kind == VerifiedMemoryAccessKind::Store;
}

ir::MemoryDepKind verifiedDependenceKind(const VerifiedMemoryAccess& source,
                                         const VerifiedMemoryAccess& destination) {
  if (verifiedIsStore(source) && verifiedIsStore(destination))
    return ir::MemoryDepKind::WAW;
  return verifiedIsStore(source) ? ir::MemoryDepKind::RAW : ir::MemoryDepKind::WAR;
}

std::optional<std::uint32_t> verifiedPositiveDistance(std::int64_t numerator, std::int64_t stride) {
  if (stride == 0 || numerator % stride != 0)
    return std::nullopt;
  const auto distance = numerator / stride;
  if (distance <= 0 ||
      static_cast<std::uint64_t>(distance) > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return static_cast<std::uint32_t>(distance);
}

void addVerifiedDependence(
    VerifiedMemoryExpectations& result,
    std::set<std::tuple<std::uint32_t, std::uint32_t, ir::MemoryDepKind, std::uint32_t>>& seen,
    std::uint32_t source, std::uint32_t destination, ir::MemoryDepKind kind, std::uint32_t distance,
    VerifiedMemoryDependenceMode mode) {
  if (!seen.emplace(source, destination, kind, distance).second)
    return;
  result.dependences.push_back({source, destination, kind, distance, mode});
}

VerifiedMemoryExpectations recomputeMemoryExpectations(const Selection& selection) {
  VerifiedMemoryExpectations result;
  auto* function = selection.loop->getHeader()->getParent();
  const auto& dataLayout = function->getParent()->getDataLayout();
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(*function);
  llvm::ScalarEvolution scalarEvolution(*function, libraryInfo, assumptions,
                                        *selection.dominatorTree, *selection.loopInfo);
  llvm::BasicAAResult basicAA(dataLayout, *function, libraryInfo, assumptions,
                              selection.dominatorTree.get());
  llvm::AAResults aliasAnalysis(libraryInfo);
  aliasAnalysis.addAAResult(basicAA);

  for (auto& block : *function) {
    if (!selection.loop->contains(&block))
      continue;
    for (const auto& instruction : block) {
      const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (!load && !store)
        continue;

      const auto* accessedType = load ? load->getType() : store->getValueOperand()->getType();
      const auto alignment = load ? load->getAlign().value() : store->getAlign().value();
      if (!accessedType->isIntegerTy(32) || alignment < 4 ||
          (load && (load->isVolatile() || load->isAtomic())) ||
          (store && (store->isVolatile() || store->isAtomic()))) {
        result.error = "LLVM memory access is outside the verified i32 V0 subset";
        return result;
      }

      const auto* address = verifiedPointerOperand(instruction);
      const auto* pointerType = llvm::dyn_cast<llvm::PointerType>(address->getType());
      const auto* base = llvm::getUnderlyingObject(address);
      if (!pointerType || pointerType->getAddressSpace() != 0 || !base ||
          !base->getType()->isPointerTy() ||
          (llvm::isa<llvm::Instruction>(base) &&
           selection.loop->contains(llvm::cast<llvm::Instruction>(base))) ||
          llvm::isa<llvm::PHINode>(base) || llvm::isa<llvm::SelectInst>(base) ||
          llvm::isa<llvm::LoadInst>(base)) {
        result.error = "LLVM memory access has no verified loop-invariant pointer root";
        return result;
      }

      VerifiedMemoryAccess access;
      access.id = static_cast<std::uint32_t>(result.accesses.size());
      access.kind = load ? VerifiedMemoryAccessKind::Load : VerifiedMemoryAccessKind::Store;
      access.instruction = &instruction;
      access.address = address;
      access.base = base;
      access.accessWidthBits = 32;

      if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address)) {
        const auto bitWidth = dataLayout.getIndexTypeSizeInBits(gep->getType());
        llvm::MapVector<llvm::Value*, llvm::APInt> variableOffsets;
        llvm::APInt constantOffset(bitWidth, 0, true);
        if (!gep->collectOffset(dataLayout, bitWidth, variableOffsets, constantOffset) ||
            constantOffset.getMinSignedBits() > 64 || constantOffset.getSExtValue() % 4 != 0 ||
            variableOffsets.size() > 1) {
          result.error = "LLVM GEP is not a verified affine word address";
          return result;
        }
        access.gepConstantOffsetWords = constantOffset.getSExtValue() / 4;
        access.constantOffsetBytes = constantOffset.getSExtValue();
        if (!verifiedFitsAddressWord(access.gepConstantOffsetWords)) {
          result.error = "LLVM GEP constant word offset exceeds the verified i32 address domain";
          return result;
        }
        access.constantOffsetWords = access.gepConstantOffsetWords;
        if (!variableOffsets.empty()) {
          const auto& [index, scaleBytes] = variableOffsets.front();
          if (scaleBytes.getMinSignedBits() > 64 || scaleBytes.getSExtValue() % 4 != 0 ||
              !verifiedFitsAddressWord(scaleBytes.getSExtValue() / 4)) {
            result.error = "dynamic GEP scale exceeds the verified i32 word-address domain";
            return result;
          }
          const auto scaleWords = scaleBytes.getSExtValue() / 4;
          const auto affine = verifiedAffineValue(*index, *selection.loop, scalarEvolution);
          if (!affine || !verifiedFitsAddressWord(affine->offset) ||
              !verifiedFitsAddressWord(affine->stride)) {
            result.error = "dynamic GEP index is not a verified affine recurrence";
            return result;
          }
          const auto derivedOffset = access.gepConstantOffsetWords + affine->offset * scaleWords;
          const auto derivedStride = affine->stride * scaleWords;
          if (!verifiedFitsAddressWord(derivedOffset) || !verifiedFitsAddressWord(derivedStride)) {
            result.error = "affine GEP exceeds the verified i32 word-address domain";
            return result;
          }
          access.dynamicIndex = index;
          access.addressMode = affine->mode;
          access.invariantExpression = affine->invariantExpression;
          access.dynamicScaleWords = scaleWords;
          access.constantOffsetWords = derivedOffset;
          access.iterationStrideWords = derivedStride;
          access.constantOffsetBytes = constantOffset.getSExtValue() +
                                       affine->offset * scaleBytes.getSExtValue();
          access.iterationStrideBytes = affine->stride * scaleBytes.getSExtValue();
        }
      }
      result.accesses.push_back(access);
    }
  }

  std::set<std::tuple<std::uint32_t, std::uint32_t, ir::MemoryDepKind, std::uint32_t>> seen;
  for (const auto& access : result.accesses)
    if (verifiedIsStore(access) && access.addressMode == LLVMAddressMode::Dynamic)
      addVerifiedDependence(result, seen, access.id, access.id, ir::MemoryDepKind::WAW, 1,
                            VerifiedMemoryDependenceMode::DynamicConservative);
    else if (verifiedIsStore(access) && access.iterationStrideWords == 0)
      addVerifiedDependence(result, seen, access.id, access.id, ir::MemoryDepKind::WAW, 1,
                            VerifiedMemoryDependenceMode::ExactAffine);

  for (std::size_t lhsIndex = 0; lhsIndex < result.accesses.size(); ++lhsIndex) {
    for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < result.accesses.size(); ++rhsIndex) {
      auto* lhs = &result.accesses[lhsIndex];
      auto* rhs = &result.accesses[rhsIndex];
      if (!verifiedIsStore(*lhs) && !verifiedIsStore(*rhs))
        continue;

      const bool exact = lhs->addressMode != LLVMAddressMode::Dynamic &&
                         rhs->addressMode != LLVMAddressMode::Dynamic && lhs->base == rhs->base &&
                         lhs->accessWidthBits == rhs->accessWidthBits &&
                         lhs->iterationStrideWords == rhs->iterationStrideWords &&
                         lhs->invariantExpression == rhs->invariantExpression;
      if (!exact && aliasAnalysis.alias(llvm::MemoryLocation::get(lhs->instruction),
                                        llvm::MemoryLocation::get(rhs->instruction)) ==
                        llvm::AliasResult::NoAlias)
        continue;
      if (!verifiedInstructionBefore(*lhs->instruction, *rhs->instruction,
                                     *selection.dominatorTree)) {
        if (verifiedInstructionBefore(*rhs->instruction, *lhs->instruction,
                                      *selection.dominatorTree))
          std::swap(lhs, rhs);
        else {
          result.error = "MayAlias accesses have no verified path-independent order";
          return result;
        }
      }

      if (exact) {
        const auto stride = lhs->iterationStrideWords;
        if (lhs->constantOffsetWords == rhs->constantOffsetWords)
          addVerifiedDependence(result, seen, lhs->id, rhs->id, verifiedDependenceKind(*lhs, *rhs),
                                0, VerifiedMemoryDependenceMode::ExactAffine);
        if (stride == 0) {
          if (lhs->constantOffsetWords == rhs->constantOffsetWords)
            addVerifiedDependence(result, seen, rhs->id, lhs->id,
                                  verifiedDependenceKind(*rhs, *lhs), 1,
                                  VerifiedMemoryDependenceMode::ExactAffine);
          continue;
        }
        if (const auto distance = verifiedPositiveDistance(
                lhs->constantOffsetWords - rhs->constantOffsetWords, stride))
          addVerifiedDependence(result, seen, lhs->id, rhs->id, verifiedDependenceKind(*lhs, *rhs),
                                *distance, VerifiedMemoryDependenceMode::ExactAffine);
        if (const auto distance = verifiedPositiveDistance(
                rhs->constantOffsetWords - lhs->constantOffsetWords, stride))
          addVerifiedDependence(result, seen, rhs->id, lhs->id, verifiedDependenceKind(*rhs, *lhs),
                                *distance, VerifiedMemoryDependenceMode::ExactAffine);
        continue;
      }

      const auto conservativeMode =
          lhs->addressMode == LLVMAddressMode::Dynamic || rhs->addressMode == LLVMAddressMode::Dynamic
              ? VerifiedMemoryDependenceMode::DynamicConservative
              : VerifiedMemoryDependenceMode::Conservative;
      addVerifiedDependence(result, seen, lhs->id, rhs->id, verifiedDependenceKind(*lhs, *rhs), 0,
                            conservativeMode);
      addVerifiedDependence(result, seen, rhs->id, lhs->id, verifiedDependenceKind(*rhs, *lhs), 1,
                            conservativeMode);
    }
  }
  return result;
}

std::string_view verifiedModeName(VerifiedMemoryDependenceMode mode) {
  switch (mode) {
  case VerifiedMemoryDependenceMode::ExactAffine:
    return "exact_affine";
  case VerifiedMemoryDependenceMode::Conservative:
    return "conservative";
  case VerifiedMemoryDependenceMode::DynamicConservative:
    return "dynamic_conservative";
  }
  return "dynamic_conservative";
}

const LLVMMemoryAccessProvenance* memoryAccessProvenance(const LLVMFrontendResult& result,
                                                         const llvm::Instruction* instruction) {
  const auto item = std::ranges::find_if(result.provenance.memoryAccesses, [&](const auto& access) {
    return access.instruction == instruction;
  });
  return item == result.provenance.memoryAccesses.end() ? nullptr : &*item;
}

bool constantBindingMatches(const ir::DFG& dfg, ir::NodeId node, std::uint32_t operand,
                            std::int64_t expected) {
  if (!verifiedFitsAddressWord(expected))
    return false;
  const auto expectedBits = static_cast<std::uint32_t>(static_cast<std::int32_t>(expected));
  for (const auto& binding : dfg.externalBindings()) {
    if (binding.node != node || binding.operand != operand)
      continue;
    const auto* reference = std::get_if<ir::ConstantRef>(&binding.source);
    if (!reference || !dfg.containsConstant(reference->value))
      return false;
    return dfg.constant(reference->value).type == ir::ValueType::i32() &&
           dfg.constant(reference->value).bits == expectedBits;
  }
  return false;
}

bool gepAddressGraphMatches(const LLVMFrontendResult& result,
                            const VerifiedMemoryAccess& descriptor, ir::NodeId addressNode) {
  if (!result.dfg->containsNode(addressNode) ||
      result.dfg->node(addressNode).opcode != ir::Opcode::Add ||
      !providerMatches(result, descriptor.base, addressNode, 0))
    return false;
  if (!descriptor.dynamicIndex)
    return constantBindingMatches(*result.dfg, addressNode, 1, descriptor.gepConstantOffsetWords);

  ir::NodeId dynamicConsumer = addressNode;
  std::uint32_t dynamicOperand = 1;
  if (descriptor.gepConstantOffsetWords != 0) {
    const auto* offsetEdge = findProviderEdge(*result.dfg, addressNode, 1, ir::Edge::Kind::Data);
    if (!offsetEdge || offsetEdge->distance != 0 ||
        result.dfg->node(offsetEdge->src).opcode != ir::Opcode::Add ||
        !constantBindingMatches(*result.dfg, offsetEdge->src, 1, descriptor.gepConstantOffsetWords))
      return false;
    dynamicConsumer = offsetEdge->src;
    dynamicOperand = 0;
  }
  if (descriptor.dynamicScaleWords == 1)
    return providerMatches(result, descriptor.dynamicIndex, dynamicConsumer, dynamicOperand);

  const auto* scaleEdge =
      findProviderEdge(*result.dfg, dynamicConsumer, dynamicOperand, ir::Edge::Kind::Data);
  if (!scaleEdge || scaleEdge->distance != 0 ||
      result.dfg->node(scaleEdge->src).opcode != ir::Opcode::Mul)
    return false;
  return providerMatches(result, descriptor.dynamicIndex, scaleEdge->src, 0) &&
         constantBindingMatches(*result.dfg, scaleEdge->src, 1, descriptor.dynamicScaleWords);
}

void verifyMemoryDataflow(const Selection& selection, const LLVMFrontendResult& result,
                          LLVMFrontendVerificationReport& report) {
  const auto expected = recomputeMemoryExpectations(selection);
  if (!expected.ok()) {
    report.add("LLVM_FRONTEND_MEMORY_ANALYSIS_VERIFY_FAILED", expected.error);
    return;
  }
  if (expected.accesses.size() != result.provenance.memoryAccesses.size())
    report.add("LLVM_FRONTEND_MEMORY_ACCESS_VERIFY_FAILED",
               "memory access provenance does not cover every LLVM Load/Store");

  std::unordered_map<std::uint32_t, const LLVMMemoryAccessProvenance*> accesses;
  for (const auto& descriptor : expected.accesses) {
    const auto* actual = memoryAccessProvenance(result, descriptor.instruction);
    if (!actual) {
      report.add("LLVM_FRONTEND_MEMORY_ACCESS_VERIFY_FAILED",
                 "LLVM memory instruction has no Generic access provenance");
      continue;
    }
    accesses.emplace(descriptor.id, actual);
    if (actual->id != descriptor.id || actual->baseValue != descriptor.base ||
        actual->addressMode != toString(descriptor.addressMode) ||
        actual->invariantExpression != descriptor.invariantExpression ||
        actual->offsetBytes != descriptor.constantOffsetBytes ||
        actual->strideBytes != descriptor.iterationStrideBytes ||
        actual->offsetWords != descriptor.constantOffsetWords ||
        actual->strideWords != descriptor.iterationStrideWords ||
        actual->accessWidthBits != descriptor.accessWidthBits ||
        !result.dfg->containsNode(actual->memoryNode)) {
      report.add("LLVM_FRONTEND_MEMORY_ACCESS_VERIFY_FAILED",
                 "memory descriptor does not match independent LLVM affine analysis");
      continue;
    }
    const auto& memoryNode = result.dfg->node(actual->memoryNode);
    const auto expectedOpcode =
        descriptor.kind == VerifiedMemoryAccessKind::Load ? ir::Opcode::Load : ir::Opcode::Store;
    if (memoryNode.opcode != expectedOpcode || !memoryNode.memoryInfo ||
        memoryNode.memoryInfo->accessWidthBits != descriptor.accessWidthBits)
      report.add("LLVM_FRONTEND_MEMORY_NODE_SEMANTICS_MISMATCH",
                 "Generic memory node opcode/width does not match LLVM");

    if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(descriptor.address)) {
      if (!gepAddressGraphMatches(result, descriptor, actual->addressProvider) ||
          !providerMatches(result, gep, actual->memoryNode, 0))
        report.add("LLVM_FRONTEND_ADDRESS_SEMANTICS_MISMATCH",
                   "Generic address graph does not implement the LLVM GEP word offset");
    } else if (!providerMatches(result, descriptor.base, actual->memoryNode, 0)) {
      report.add("LLVM_FRONTEND_ADDRESS_SEMANTICS_MISMATCH",
                 "direct memory address does not use the canonical pointer base");
    }
    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(descriptor.instruction))
      if (!providerMatches(result, store->getValueOperand(), actual->memoryNode, 1))
        report.add("LLVM_FRONTEND_MEMORY_ACCESS_VERIFY_FAILED",
                   "Store data provider does not match LLVM");
  }

  std::unordered_set<ir::EdgeId> expectedEdges;
  for (const auto& dependence : expected.dependences) {
    if (!accesses.contains(dependence.sourceAccess) ||
        !accesses.contains(dependence.destinationAccess))
      continue;
    const auto source = accesses.at(dependence.sourceAccess)->memoryNode;
    const auto destination = accesses.at(dependence.destinationAccess)->memoryNode;
    const ir::Edge* matching = nullptr;
    for (const auto& edge : result.dfg->edges()) {
      const auto* memory = std::get_if<ir::MemoryEdgeInfo>(&edge.info);
      if (memory && edge.src == source && edge.dst == destination &&
          memory->dependence == dependence.kind && edge.distance == dependence.distance) {
        if (matching)
          report.add("LLVM_FRONTEND_MEMORY_DEPENDENCE_VERIFY_FAILED",
                     "duplicate Generic MemoryEdge for one LLVM dependence");
        matching = &edge;
      }
    }
    if (!matching) {
      report.add("LLVM_FRONTEND_MEMORY_DEPENDENCE_VERIFY_FAILED",
                 "required RAW/WAR/WAW dependence is missing or corrupted");
      continue;
    }
    expectedEdges.insert(matching->id);
    const auto provenance =
        std::ranges::find_if(result.provenance.memoryDependences, [&](const auto& item) {
          return item.edge == matching->id && item.sourceAccess == dependence.sourceAccess &&
                 item.destinationAccess == dependence.destinationAccess &&
                 item.kind == ir::toString(dependence.kind) &&
                 item.distance == dependence.distance &&
                 item.mode == verifiedModeName(dependence.mode);
        });
    if (provenance == result.provenance.memoryDependences.end())
      report.add("LLVM_FRONTEND_MEMORY_DEPENDENCE_VERIFY_FAILED",
                 "MemoryEdge provenance does not match independent dependence analysis");
  }
  for (const auto& edge : result.dfg->edges())
    if (edge.kind() == ir::Edge::Kind::Memory && !expectedEdges.contains(edge.id))
      report.add("LLVM_FRONTEND_MEMORY_DEPENDENCE_VERIFY_FAILED",
                 "spurious Generic MemoryEdge has no LLVM dependence");
  if (expected.dependences.size() != result.provenance.memoryDependences.size())
    report.add("LLVM_FRONTEND_MEMORY_DEPENDENCE_VERIFY_FAILED",
               "memory dependence provenance count is inconsistent");
}

LLVMFrontendVerificationReport verifyIfConvertedResult(const llvm::Module& module,
                                                       const LLVMFrontendOptions& options,
                                                       const LLVMFrontendResult& result) {
  LLVMFrontendVerificationReport report;
  const auto selection = select(module, options, report);
  if (!selection)
    return report;
  const auto dfgReport = ir::DFGVerifier::verify(*result.dfg);
  if (!dfgReport.ok())
    report.add("LLVM_FRONTEND_DFG_VERIFY_FAILED", dfgReport.format());
  verifyIfRegionStructure(*selection, result, report);
  verifyLinearRegionStructure(*selection, result, report);
  verifyIfDataflow(*selection, result, report);
  verifyMemoryDataflow(*selection, result, report);

  for (const auto& node : result.dfg->nodes()) {
    const auto* provenance = nodeProvenance(result, node.id);
    if (!provenance || !provenance->instruction) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_MISSING",
                 "Generic predication node has no LLVM provenance");
      continue;
    }
    const auto* instruction = provenance->instruction;
    if (!selection->loop->contains(instruction) && !llvm::isa<llvm::PHINode>(instruction))
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_INVALID",
                 "predication node provenance is outside the selected loop");
    if (llvm::isa<llvm::ICmpInst>(instruction)) {
      const auto* compare = llvm::cast<llvm::ICmpInst>(instruction);
      const auto predicate = icmpPredicate(*compare);
      bool complemented = false;
      for (const auto& region : result.provenance.ifConversions)
        complemented |= region.conditionValue == compare && region.predicateComplemented;
      const auto normalized =
          predicate && complemented ? complementPredicate(*predicate) : predicate;
      if (node.opcode != ir::Opcode::ICmp || !normalized || node.icmpPredicate != normalized ||
          node.resultType != ir::ValueType::predicate())
        report.add("LLVM_FRONTEND_ICMP_SEMANTICS_MISMATCH",
                   "Generic ICmp does not match the LLVM predicate");
    } else if (llvm::isa<llvm::SelectInst>(instruction) || llvm::isa<llvm::PHINode>(instruction)) {
      if (node.opcode != ir::Opcode::Select)
        report.add("LLVM_FRONTEND_SELECT_SEMANTICS_MISMATCH",
                   "control merge must lower to Generic Select");
    } else if (llvm::isa<llvm::StoreInst>(instruction)) {
      if (node.opcode != ir::Opcode::Store)
        report.add("LLVM_FRONTEND_STORE_SEMANTICS_MISMATCH",
                   "predicated Store provenance does not identify a Store node");
    }
  }

  for (const auto& region : result.provenance.ifConversions) {
    if (!region.conditionValue) {
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED", "if-conversion region has no condition");
      continue;
    }
    const auto* predicateProvenance = nodeProvenance(result, region.predicateNode);
    if (!predicateProvenance || predicateProvenance->instruction != region.conditionValue) {
      report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                 "predicate node does not correspond to the branch condition");
    } else if (const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(region.conditionValue)) {
      const auto expected = icmpPredicate(*compare);
      const auto normalized =
          expected && region.predicateComplemented ? complementPredicate(*expected) : expected;
      if (!normalized || result.dfg->node(region.predicateNode).icmpPredicate != normalized)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicate polarity does not match the normalized branch condition");
      if (expected) {
        const bool swap = region.predicateComplemented && complementSwapsOperands(*expected);
        const auto* first = compare->getOperand(swap ? 1 : 0);
        const auto* second = compare->getOperand(swap ? 0 : 1);
        if (!providerMatches(result, first, region.predicateNode, 0) ||
            !providerMatches(result, second, region.predicateNode, 1))
          report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                     "predicate operands do not match normalized branch condition");
      }
    }
    for (const auto& select : region.selects) {
      if (!result.dfg->containsNode(select.node)) {
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED", "if-conversion references missing Select");
        continue;
      }
      const auto& node = result.dfg->node(select.node);
      if (node.opcode != ir::Opcode::Select) {
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED", "if-conversion node is not Select");
        continue;
      }
      const auto* predicateEdge =
          findProviderEdge(*result.dfg, select.node, 0, ir::Edge::Kind::Predicate);
      if (!predicateEdge || predicateEdge->src != region.predicateNode)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "Select predicate provider does not match branch condition");
      for (std::uint32_t operand = 1; operand < 3; ++operand)
        if (!findProviderEdge(*result.dfg, select.node, operand, ir::Edge::Kind::Data)) {
          bool bound = false;
          for (const auto& binding : result.dfg->externalBindings())
            bound |= binding.node == select.node && binding.operand == operand;
          if (!bound)
            report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED", "Select arm has no value provider");
        }
      if (!providerMatches(result, select.trueProvider, select.node, 1) ||
          !providerMatches(result, select.falseProvider, select.node, 2))
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "Select arm providers do not match LLVM predecessor values");
    }
    for (const auto storeNode : region.predicatedStores) {
      if (!result.dfg->containsNode(storeNode) ||
          result.dfg->node(storeNode).opcode != ir::Opcode::Store) {
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED", "predicated Store provenance is invalid");
        continue;
      }
      const auto* predicateEdge =
          findProviderEdge(*result.dfg, storeNode, 2, ir::Edge::Kind::Predicate);
      if (!predicateEdge || predicateEdge->src != region.predicateNode)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicated Store commit predicate does not match branch condition");
      const auto* llvmStore = llvm::dyn_cast<llvm::StoreInst>(
          nodeProvenance(result, storeNode) ? nodeProvenance(result, storeNode)->instruction
                                            : nullptr);
      const auto* normalizedTrueBlock =
          region.branch ? region.branch->getSuccessor(region.predicateComplemented ? 1 : 0)
                        : nullptr;
      if (!llvmStore || llvmStore->getParent() != normalizedTrueBlock)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicated Store is not guarded by the normalized true branch arm");
      if (llvmStore && (!providerMatches(result, llvmStore->getPointerOperand(), storeNode, 0) ||
                        !providerMatches(result, llvmStore->getValueOperand(), storeNode, 1)))
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicated Store address or data provider does not match LLVM Store");
      bool sawWaw = false;
      for (const auto edgeId : result.dfg->outgoing(storeNode)) {
        const auto& edge = result.dfg->edge(edgeId);
        sawWaw |= edge.kind() == ir::Edge::Kind::Memory && edge.dst == storeNode &&
                  std::get<ir::MemoryEdgeInfo>(edge.info).dependence == ir::MemoryDepKind::WAW &&
                  edge.distance == 1;
      }
      const auto* memory = llvmStore ? memoryAccessProvenance(result, llvmStore) : nullptr;
      if (memory && (memory->strideWords == 0) != sawWaw)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicated Store self-WAW does not match invariant-address semantics");
    }
  }
  return report;
}

} // namespace

void LLVMFrontendVerificationReport::add(std::string code, std::string message) {
  diagnostics_.push_back({std::move(code), std::move(message)});
}

std::string LLVMFrontendVerificationReport::format() const {
  std::ostringstream output;
  output << (ok() ? "valid" : "invalid") << " LLVM frontend result";
  for (const auto& diagnostic : diagnostics_)
    output << '\n' << diagnostic.code << ": " << diagnostic.message;
  return output.str();
}

std::string LLVMFrontendVerificationReport::toJson() const {
  Json root{{"schema", "cgra.llvm_frontend.verification.v1"},
            {"valid", ok()},
            {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics_)
    root["diagnostics"].push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
  return root.dump(2) + '\n';
}

LLVMFrontendVerificationReport verifyFrontendResult(const llvm::Module& module,
                                                    const LLVMFrontendOptions& options,
                                                    const LLVMFrontendResult& result) {
  LLVMFrontendVerificationReport report;
  if (!result.ok()) {
    report.add("LLVM_FRONTEND_RESULT_NOT_SUCCESS", "frontend result is not successful");
    return report;
  }
  const auto originalSelection = select(module, options, report);
  if (!originalSelection)
    return report;
  const bool originalNeedsCanonicalEntry = originalSelection->loop->getLoopPreheader() == nullptr;
  if (!result.metadata ||
      result.metadata->loopEntryCanonicalized != originalNeedsCanonicalEntry) {
    report.add("LLVM_FRONTEND_LOOP_ENTRY_VERIFY_FAILED",
               "loop-entry canonicalization metadata does not match the original LLVM CFG");
  }
  if (originalNeedsCanonicalEntry && !result.normalizedModule) {
    report.add("LLVM_FRONTEND_LOOP_ENTRY_VERIFY_FAILED",
               "entry canonicalization did not retain the normalized LLVM module");
    return report;
  }
  const llvm::Module& verificationModule =
      result.normalizedModule ? *result.normalizedModule : module;
  const auto selection = select(verificationModule, options, report);
  if (!selection)
    return report;
  if (originalNeedsCanonicalEntry && !selection->loop->getLoopPreheader()) {
    report.add("LLVM_FRONTEND_LOOP_ENTRY_VERIFY_FAILED",
               "normalized selected loop still has no unique preheader");
    return report;
  }
  if (!result.provenance.ifConversions.empty() || !result.provenance.memoryAccesses.empty() ||
      result.provenance.linearLoop)
    return verifyIfConvertedResult(verificationModule, options, result);
  if (!selection->branch || selection->loop->getBlocks().size() != 1) {
    report.add("LLVM_FRONTEND_LOOP_SHAPE_MISMATCH", "selected loop shape changed after lowering");
    return report;
  }
  auto slice = controlSlice(*selection);
  removeRecurrenceProducerClosure(*selection, slice);
  const auto dfgReport = ir::DFGVerifier::verify(*result.dfg);
  if (!dfgReport.ok())
    report.add("LLVM_FRONTEND_DFG_VERIFY_FAILED", dfgReport.format());

  std::unordered_set<const llvm::Instruction*> mappedInstructions;
  for (const auto& node : result.dfg->nodes()) {
    const auto* provenance = nodeProvenance(result, node.id);
    if (!provenance || !provenance->instruction) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_MISSING",
                 "Generic node has no LLVM instruction provenance");
      continue;
    }
    const auto* instruction = provenance->instruction;
    if (!selection->loop->contains(instruction) || slice.contains(instruction) ||
        ignored(*instruction)) {
      report.add("LLVM_FRONTEND_NODE_PROVENANCE_INVALID",
                 "node provenance is outside the selected data path");
      continue;
    }
    const auto expectedOpcode = opcode(*instruction);
    const auto expectedType = valueType(*instruction);
    if (!expectedOpcode || !expectedType || node.opcode != *expectedOpcode ||
        node.resultType != *expectedType) {
      report.add("LLVM_FRONTEND_NODE_SEMANTICS_MISMATCH",
                 "Generic node opcode or result type does not match LLVM instruction");
    }
    mappedInstructions.insert(instruction);
  }

  for (const auto& edge : result.dfg->edges()) {
    if (edge.kind() != ir::Edge::Kind::Data) {
      report.add("LLVM_FRONTEND_EDGE_SEMANTICS_MISMATCH",
                 "LLVM frontend recurrence edges must be Data edges");
      continue;
    }
    const auto* source = nodeProvenance(result, edge.src);
    const auto* destination = nodeProvenance(result, edge.dst);
    if (!source || !destination || !source->instruction || !destination->instruction) {
      report.add("LLVM_FRONTEND_EDGE_PROVENANCE_MISSING", "edge endpoint provenance is missing");
      continue;
    }
    const auto info = std::get<ir::DataEdgeInfo>(edge.info);
    if (edge.distance == 0) {
      if (info.boundary || info.dstOperand >= destination->instruction->getNumOperands() ||
          destination->instruction->getOperand(info.dstOperand) != source->instruction) {
        report.add("LLVM_FRONTEND_EDGE_PROVENANCE_INVALID",
                   "Generic data edge does not match the LLVM SSA def-use operand");
      }
      continue;
    }
    if (edge.distance != 1 || !info.boundary) {
      report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                 "recurrence edge must have distance one and a boundary");
      continue;
    }
    const auto* recurrence =
        info.dstOperand < destination->instruction->getNumOperands()
            ? recurrenceForEdge(result, source->instruction,
                                destination->instruction->getOperand(info.dstOperand))
            : nullptr;
    if (!recurrence || !recurrence->phiValue ||
        info.dstOperand >= destination->instruction->getNumOperands() ||
        destination->instruction->getOperand(info.dstOperand) != recurrence->phiValue ||
        !boundaryMatches(result, *result.dfg, *recurrence, *info.boundary)) {
      report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                 "distance-one edge does not match a canonical LLVM PHI use and boundary");
    }
  }

  std::unordered_set<const llvm::PHINode*> verifiedRecurrencePhis;
  for (const auto& recurrence : result.provenance.recurrences) {
    if (!recurrence.phiValue || !recurrence.backedge || recurrence.distance != 1) {
      report.add("LLVM_FRONTEND_RECURRENCE_BOUNDARY_VERIFY_FAILED",
                 "recurrence descriptor is incomplete");
      continue;
    }
    verifiedRecurrencePhis.insert(recurrence.phiValue);
    for (const auto& use : recurrence.uses) {
      if (!result.dfg->containsEdge(use.edge)) {
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence descriptor references a missing Generic edge");
        continue;
      }
      const auto& edge = result.dfg->edge(use.edge);
      const auto* source = nodeProvenance(result, edge.src);
      const auto* destination = nodeProvenance(result, edge.dst);
      if (edge.kind() != ir::Edge::Kind::Data || edge.distance != 1 ||
          edge.dst != use.destination || !source || !destination ||
          source->instruction != recurrence.backedge ||
          std::get<ir::DataEdgeInfo>(edge.info).dstOperand != use.operand ||
          !destination->instruction ||
          destination->instruction->getOperand(use.operand) != recurrence.phiValue ||
          !std::get<ir::DataEdgeInfo>(edge.info).boundary ||
          !boundaryMatches(result, *result.dfg, recurrence,
                           *std::get<ir::DataEdgeInfo>(edge.info).boundary)) {
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence descriptor edge identity is inconsistent");
      }
    }
  }
  for (const auto& recurrence : result.provenance.recurrences) {
    if (!recurrence.phiValue || !verifiedRecurrencePhis.contains(recurrence.phiValue))
      continue;
    bool sawEdge = false;
    for (const auto& edge : result.dfg->edges()) {
      if (edge.distance != 1 || edge.kind() != ir::Edge::Kind::Data)
        continue;
      const auto& info = std::get<ir::DataEdgeInfo>(edge.info);
      const auto* destination = nodeProvenance(result, edge.dst);
      const auto* source = nodeProvenance(result, edge.src);
      if (!destination || !source || !destination->instruction || !source->instruction)
        continue;
      if (source->instruction == recurrence.backedge &&
          info.dstOperand < destination->instruction->getNumOperands() &&
          destination->instruction->getOperand(info.dstOperand) == recurrence.phiValue) {
        sawEdge = true;
        break;
      }
    }
    if (!sawEdge)
      report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                 "recurrence PHI has no corresponding Generic recurrence edge");

    for (const auto* user : recurrence.phiValue->users()) {
      const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (!instruction || !selection->loop->contains(instruction) || slice.contains(instruction) ||
          instruction->isTerminator() || llvm::isa<llvm::ICmpInst>(instruction) ||
          ignored(*instruction))
        continue;
      const LLVMFrontendNodeProvenance* destination = nullptr;
      bool found = false;
      for (const auto& node : result.dfg->nodes()) {
        const auto* candidate = nodeProvenance(result, node.id);
        if (candidate && candidate->instruction == instruction) {
          destination = candidate;
          break;
        }
      }
      if (!destination) {
        report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                   "recurrence PHI data use has no Generic destination node");
        continue;
      }
      for (unsigned operand = 0; operand < instruction->getNumOperands(); ++operand) {
        if (instruction->getOperand(operand) != recurrence.phiValue)
          continue;
        for (const auto& edge : result.dfg->edges()) {
          if (edge.distance != 1 || edge.kind() != ir::Edge::Kind::Data ||
              edge.dst != destination->node)
            continue;
          const auto& info = std::get<ir::DataEdgeInfo>(edge.info);
          const auto* source = nodeProvenance(result, edge.src);
          if (source && source->instruction == recurrence.backedge && info.dstOperand == operand &&
              info.boundary && boundaryMatches(result, *result.dfg, recurrence, *info.boundary)) {
            found = true;
            break;
          }
        }
        if (!found)
          report.add("LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED",
                     "recurrence PHI data use is missing its Generic edge");
      }
    }
  }

  std::unordered_set<const llvm::Value*> externalValues;
  for (const auto& external : result.dfg->externalValues()) {
    const LLVMFrontendExternalProvenance* provenance = nullptr;
    for (const auto& candidate : result.provenance.externals)
      if (candidate.external == external.id)
        provenance = &candidate;
    if (!provenance || !provenance->value) {
      report.add("LLVM_FRONTEND_EXTERNAL_PROVENANCE_MISSING",
                 "ExternalValue has no LLVM value provenance");
      continue;
    }
    const auto* definingInstruction = llvm::dyn_cast<llvm::Instruction>(provenance->value);
    if ((definingInstruction && selection->loop->contains(definingInstruction)) ||
        llvm::isa<llvm::ConstantInt>(provenance->value) || !valueType(*provenance->value) ||
        !externalValues.insert(provenance->value).second) {
      report.add("LLVM_FRONTEND_EXTERNAL_PROVENANCE_INVALID",
                 "ExternalValue provenance is not a unique loop-external scalar");
    }
  }

  for (const auto& liveOut : result.dfg->liveOuts()) {
    const LLVMFrontendLiveOutProvenance* provenance = nullptr;
    for (const auto& candidate : result.provenance.liveOuts)
      if (candidate.liveOut == liveOut.id)
        provenance = &candidate;
    const auto* source = nodeProvenance(result, liveOut.source);
    if (!provenance || !provenance->value || !source || source->instruction != provenance->value ||
        !hasOutsideUse(*provenance->value, *selection)) {
      report.add("LLVM_FRONTEND_LIVEOUT_PROVENANCE_INVALID",
                 "LiveOut does not identify an in-loop value with an outside use");
    }
  }

  for (const auto& instruction : *selection->block) {
    if (instruction.isTerminator() || ignored(instruction) ||
        llvm::isa<llvm::PHINode>(instruction) || slice.contains(&instruction))
      continue;
    if (!mappedInstructions.contains(&instruction))
      report.add("LLVM_FRONTEND_SILENT_INSTRUCTION_LOSS",
                 "semantic LLVM instruction is absent from Generic DFG provenance");
  }
  return report;
}

} // namespace cgra::frontend::llvm_frontend
