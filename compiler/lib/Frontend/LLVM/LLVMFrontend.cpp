// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ReplaceConstant.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace cgra::frontend::llvm_frontend {
namespace {

using Json = nlohmann::json;

struct LoopSelection {
  llvm::Function* function = nullptr;
  llvm::Loop* loop = nullptr;
  llvm::BasicBlock* block = nullptr;
  llvm::BasicBlock* exit = nullptr;
  llvm::BasicBlock* preheader = nullptr;
  llvm::BranchInst* branch = nullptr;
  std::optional<LinearLoopRegionDescriptor> linearRegion;
  bool loopEntryCanonicalized = false;
  std::uint32_t coalescedStorePairs = 0;
  std::uint32_t forwardedBranchLoads = 0;
  std::unique_ptr<llvm::DominatorTree> dominatorTree;
  std::unique_ptr<llvm::LoopInfo> loopInfo;
};

struct LoweringState {
  LoopSelection selection;
  std::unordered_set<const llvm::Instruction*> controlSlice;
  std::unordered_map<const llvm::Value*, ir::NodeId> nodes;
  std::unordered_map<const llvm::Value*, ir::ExternalValueId> externals;
  std::unordered_map<const llvm::PHINode*, std::size_t> recurrences;
  std::unordered_set<const llvm::Instruction*> recurrenceBackedges;
  std::map<std::tuple<ir::ValueKind, std::uint16_t, std::uint64_t>, ir::ConstantId> constants;
  std::set<std::string> externalNames;
  std::set<std::string> liveOutNames;
  std::uint32_t nextExternalOrdinal = 0;
  LLVMFrontendProvenance provenance;
};

struct BranchRegion {
  llvm::BranchInst* branch = nullptr;
  llvm::BasicBlock* conditionBlock = nullptr;
  llvm::BasicBlock* trueBlock = nullptr;
  llvm::BasicBlock* falseBlock = nullptr;
  llvm::BasicBlock* mergeBlock = nullptr;
  const llvm::ICmpInst* condition = nullptr;
};

bool ignoredInstruction(const llvm::Instruction& instruction);
std::optional<ir::ValueType> addressValueType(const llvm::Module& module,
                                              const llvm::Value& address);

constexpr std::size_t SmallPureHelperInstructionLimit = 32;

bool isSmallPureHelper(const llvm::Function& function) {
  if (function.isDeclaration() || function.isVarArg() || function.empty())
    return false;
  std::size_t instructionCount = 0;
  for (const auto& block : function) {
    for (const auto& instruction : block) {
      if (ignoredInstruction(instruction))
        continue;
      if (++instructionCount > SmallPureHelperInstructionLimit ||
          llvm::isa<llvm::CallBase>(instruction) || llvm::isa<llvm::AllocaInst>(instruction) ||
          instruction.mayReadOrWriteMemory() || instruction.mayThrow())
        return false;
    }
  }
  return true;
}

std::uint32_t inlineSmallPureHelpers(llvm::Module& module) {
  std::vector<llvm::CallBase*> candidates;
  for (auto& function : module) {
    for (auto& block : function) {
      for (auto& instruction : block) {
        auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        auto* callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || callee == &function || llvm::isa<llvm::InvokeInst>(call) ||
            !isSmallPureHelper(*callee))
          continue;
        candidates.push_back(call);
      }
    }
  }

  std::uint32_t inlined = 0;
  for (auto* call : candidates) {
    llvm::InlineFunctionInfo information;
    if (llvm::InlineFunction(*call, information).isSuccess())
      ++inlined;
  }
  return inlined;
}

void materializeConstantAddressExpressions(llvm::Module& module) {
  std::vector<llvm::Instruction*> instructions;
  for (auto& function : module)
    for (auto& block : function)
      for (auto& instruction : block)
        instructions.push_back(&instruction);
  for (auto* instruction : instructions) {
    std::vector<llvm::ConstantExpr*> addresses;
    for (auto& operand : instruction->operands())
      if (auto* expression = llvm::dyn_cast<llvm::ConstantExpr>(operand.get());
          expression && expression->getOpcode() == llvm::Instruction::GetElementPtr)
        addresses.push_back(expression);
    for (auto* address : addresses)
      llvm::convertConstantExprsToInstructions(instruction, address);
  }
}

bool sameAddress(const llvm::Value& lhs, const llvm::Value& rhs, LoopSelection& selection) {
  if (&lhs == &rhs)
    return true;
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(*selection.function);
  llvm::ScalarEvolution scalarEvolution(*selection.function, libraryInfo, assumptions,
                                        *selection.dominatorTree, *selection.loopInfo);
  return scalarEvolution.getSCEV(const_cast<llvm::Value*>(&lhs)) ==
         scalarEvolution.getSCEV(const_cast<llvm::Value*>(&rhs));
}

bool hasWriteAfter(const llvm::Instruction& instruction) {
  for (auto iterator = std::next(instruction.getIterator());
       iterator != instruction.getParent()->end(); ++iterator)
    if (iterator->mayWriteToMemory())
      return true;
  return false;
}

bool hasWriteBefore(const llvm::Instruction& instruction) {
  for (auto iterator = instruction.getParent()->begin(); iterator != instruction.getIterator();
       ++iterator)
    if (iterator->mayWriteToMemory())
      return true;
  return false;
}

void forwardRedundantBranchLoads(LoopSelection& selection, const BranchRegion& region) {
  std::vector<llvm::LoadInst*> dominatingLoads;
  for (auto& instruction : *region.conditionBlock)
    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        load && !load->isVolatile() && !load->isAtomic() && !hasWriteAfter(*load))
      dominatingLoads.push_back(load);

  std::vector<llvm::BasicBlock*> arms{region.trueBlock, region.falseBlock};
  std::ranges::sort(arms);
  arms.erase(std::unique(arms.begin(), arms.end()), arms.end());
  for (auto* arm : arms) {
    if (!arm || arm == region.conditionBlock || arm == region.mergeBlock)
      continue;
    std::vector<llvm::LoadInst*> redundantLoads;
    for (auto& instruction : *arm) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (!load || load->isVolatile() || load->isAtomic() || hasWriteBefore(*load))
        continue;
      const auto candidate = std::ranges::find_if(dominatingLoads, [&](const auto* dominating) {
        return dominating->getType() == load->getType() &&
               sameAddress(*dominating->getPointerOperand(), *load->getPointerOperand(), selection);
      });
      if (candidate == dominatingLoads.end())
        continue;
      load->replaceAllUsesWith(*candidate);
      redundantLoads.push_back(load);
    }
    for (auto* load : redundantLoads) {
      llvm::RecursivelyDeleteTriviallyDeadInstructions(load);
      ++selection.forwardedBranchLoads;
    }
  }
}

void coalesceSameAddressStores(LoopSelection& selection, const BranchRegion& region) {
  auto storesIn = [](llvm::BasicBlock& block) {
    std::vector<llvm::StoreInst*> stores;
    for (auto& instruction : block)
      if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        stores.push_back(store);
    return stores;
  };
  auto trueStores = storesIn(*region.trueBlock);
  auto falseStores = storesIn(*region.falseBlock);
  if (trueStores.size() != 1 || falseStores.size() != 1)
    return;
  auto* trueStore = trueStores.front();
  auto* falseStore = falseStores.front();
  if (trueStore->isVolatile() || trueStore->isAtomic() || falseStore->isVolatile() ||
      falseStore->isAtomic() ||
      trueStore->getValueOperand()->getType() != falseStore->getValueOperand()->getType() ||
      !sameAddress(*trueStore->getPointerOperand(), *falseStore->getPointerOperand(), selection))
    return;
  const auto unsafeSideEffect = [&](const llvm::BasicBlock& block,
                                    const llvm::StoreInst* accepted) {
    return std::ranges::any_of(block, [&](const llvm::Instruction& instruction) {
      return &instruction != accepted && !instruction.isTerminator() &&
             instruction.mayHaveSideEffects();
    });
  };
  if (unsafeSideEffect(*region.trueBlock, trueStore) ||
      unsafeSideEffect(*region.falseBlock, falseStore))
    return;

  auto insertion = region.mergeBlock->getFirstInsertionPt();
  if (insertion == region.mergeBlock->end())
    return;
  auto* selected =
      llvm::SelectInst::Create(region.branch->getCondition(), trueStore->getValueOperand(),
                               falseStore->getValueOperand(), "cgra.store.value", &*insertion);
  auto* mergedStore = new llvm::StoreInst(selected, trueStore->getPointerOperand(), &*insertion);
  mergedStore->setAlignment(std::min(trueStore->getAlign(), falseStore->getAlign()));
  auto* trueAddress = trueStore->getPointerOperand();
  auto* falseAddress = falseStore->getPointerOperand();
  trueStore->eraseFromParent();
  falseStore->eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(trueAddress);
  llvm::RecursivelyDeleteTriviallyDeadInstructions(falseAddress);
  ++selection.coalescedStorePairs;
}

LLVMFrontendResult failure(LLVMFrontendStatus status, LLVMFrontendDiagnosticCode code,
                           std::string message, const LoopSelection* selection = nullptr,
                           const llvm::Instruction* instruction = nullptr) {
  LLVMFrontendResult result;
  result.status = status;
  result.message = message;
  LLVMFrontendDiagnostic diagnostic{code, std::move(message), {}, {}, {}};
  if (selection && selection->function)
    diagnostic.function = selection->function->getName().str();
  if (selection && selection->block)
    diagnostic.loopHeader = selection->block->getName().str();
  if (instruction) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    instruction->print(stream);
    diagnostic.instruction = stream.str();
  }
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

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

std::optional<ir::ValueType> valueType(const llvm::Value& value) {
  if (value.getType()->isFloatTy())
    return ir::ValueType::floating(32);
  if (value.getType()->isDoubleTy())
    return ir::ValueType::floating(64);
  const auto* integer = llvm::dyn_cast<llvm::IntegerType>(value.getType());
  if (!integer || integer->getBitWidth() == 0 || integer->getBitWidth() > 64)
    return std::nullopt;
  if (integer->getBitWidth() == 1)
    return ir::ValueType::predicate();
  return ir::ValueType::integer(static_cast<std::uint16_t>(integer->getBitWidth()));
}

std::optional<std::uint64_t> constantBits(const llvm::Value& value) {
  if (const auto* integer = llvm::dyn_cast<llvm::ConstantInt>(&value))
    return integer->getValue().getZExtValue();
  if (const auto* floating = llvm::dyn_cast<llvm::ConstantFP>(&value))
    return floating->getValueAPF().bitcastToAPInt().getZExtValue();
  return std::nullopt;
}

std::optional<ir::Opcode> opcode(const llvm::Instruction& instruction);

std::optional<std::string> customOperationKey(const llvm::Instruction& instruction) {
  if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      intrinsic && intrinsic->getIntrinsicID() == llvm::Intrinsic::fmuladd)
    return "FMA";
  if ((instruction.getOpcode() == llvm::Instruction::And ||
       instruction.getOpcode() == llvm::Instruction::Or ||
       instruction.getOpcode() == llvm::Instruction::Xor) &&
      instruction.getType()->isIntegerTy(1)) {
    if (instruction.getOpcode() == llvm::Instruction::And)
      return "PAND";
    if (instruction.getOpcode() == llvm::Instruction::Or)
      return "POR";
    return "PXOR";
  }
  switch (instruction.getOpcode()) {
  case llvm::Instruction::FAdd:
    return "FADD";
  case llvm::Instruction::FSub:
    return "FSUB";
  case llvm::Instruction::FMul:
    return "FMUL";
  case llvm::Instruction::FDiv:
    return "FDIV";
  case llvm::Instruction::FNeg:
    return "FNEG";
  case llvm::Instruction::SDiv:
    return "SDIV";
  case llvm::Instruction::SRem:
    return "SREM";
  case llvm::Instruction::UDiv:
    return "UDIV";
  case llvm::Instruction::URem:
    return "UREM";
  case llvm::Instruction::Trunc:
    return "TRUNC";
  case llvm::Instruction::ZExt:
    return instruction.getOperand(0)->getType()->isIntegerTy(1) ? "PZEXT" : "ZEXT";
  case llvm::Instruction::SExt:
    return "SEXT";
  case llvm::Instruction::SIToFP:
    return "SITOFP";
  case llvm::Instruction::UIToFP:
    return "UITOFP";
  case llvm::Instruction::FPToSI:
    return "FPTOSI";
  case llvm::Instruction::FPToUI:
    return "FPTOUI";
  case llvm::Instruction::FPTrunc:
    return "FPTRUNC";
  case llvm::Instruction::FPExt:
    return "FPEXT";
  case llvm::Instruction::PtrToInt:
    return "PTRTOINT";
  default:
    return std::nullopt;
  }
}

std::vector<const llvm::Value*> semanticOperands(const llvm::Instruction& instruction) {
  std::vector<const llvm::Value*> result;
  if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    result.reserve(call->arg_size());
    for (const auto& argument : call->args())
      result.push_back(argument.get());
    return result;
  }
  result.reserve(instruction.getNumOperands());
  for (const auto& operand : instruction.operands())
    result.push_back(operand.get());
  return result;
}

bool supportedDataInstruction(const llvm::Instruction& instruction) {
  return opcode(instruction).has_value() || customOperationKey(instruction).has_value();
}

bool supportedRecurrenceProducer(const llvm::Instruction& instruction) {
  return supportedDataInstruction(instruction) ||
         llvm::isa<llvm::LoadInst, llvm::SelectInst>(&instruction);
}

std::optional<ir::Opcode> opcode(const llvm::Instruction& instruction) {
  if ((instruction.getOpcode() == llvm::Instruction::And ||
       instruction.getOpcode() == llvm::Instruction::Or ||
       instruction.getOpcode() == llvm::Instruction::Xor) &&
      instruction.getType()->isIntegerTy(1))
    return std::nullopt;
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

std::string valueSummary(const llvm::Value& value) {
  if (value.hasName())
    return "%" + value.getName().str();
  if (const auto* constant = llvm::dyn_cast<llvm::Constant>(&value)) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    constant->print(stream);
    return stream.str();
  }
  if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(&value))
    return instruction->getOpcodeName();
  return "external";
}

bool ignoredInstruction(const llvm::Instruction& instruction) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(instruction))
    return true;
  if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction)) {
    return intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
           intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end;
  }
  return false;
}

bool isMemoryInstruction(const llvm::Instruction& instruction) {
  return llvm::isa<llvm::LoadInst>(instruction) || llvm::isa<llvm::StoreInst>(instruction) ||
         llvm::isa<llvm::AtomicRMWInst>(instruction) ||
         llvm::isa<llvm::AtomicCmpXchgInst>(instruction) ||
         llvm::isa<llvm::FenceInst>(instruction) || llvm::isa<llvm::GetElementPtrInst>(instruction);
}

bool isPureInstruction(const llvm::Instruction& instruction) {
  return opcode(instruction).has_value() || customOperationKey(instruction).has_value() ||
         llvm::isa<llvm::ICmpInst>(instruction) || llvm::isa<llvm::SelectInst>(instruction);
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

bool recurrenceFailure(LLVMFrontendResult& error, LLVMFrontendStatus status,
                       LLVMFrontendDiagnosticCode code, std::string message,
                       const LoopSelection& selection, const llvm::Instruction* instruction) {
  error = failure(status, code, std::move(message), &selection, instruction);
  return false;
}

std::vector<llvm::Loop*> innermostLoops(llvm::LoopInfo& loopInfo) {
  std::vector<llvm::Loop*> result;
  std::function<void(llvm::Loop*)> visit = [&](llvm::Loop* loop) {
    if (loop->isInnermost()) {
      result.push_back(loop);
      return;
    }
    for (auto* child : *loop)
      visit(child);
  };
  for (auto* loop : loopInfo)
    visit(loop);
  return result;
}

std::string loopHeaderName(const llvm::Function& function, const llvm::Loop& loop) {
  return blockName(function, *loop.getHeader());
}

std::optional<LoopSelection> selectLoop(llvm::Module& module, const LLVMFrontendOptions& options,
                                        LLVMFrontendResult& error) {
  llvm::Function* function = module.getFunction(options.functionName);
  if (!function) {
    error = failure(LLVMFrontendStatus::FunctionNotFound,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_FUNCTION_NOT_FOUND,
                    "function not found: " + options.functionName);
    return std::nullopt;
  }
  auto dominatorTree = std::make_unique<llvm::DominatorTree>(*function);
  auto loopInfo = std::make_unique<llvm::LoopInfo>(*dominatorTree);
  auto loops = innermostLoops(*loopInfo);
  if (loops.empty()) {
    LoopSelection context;
    context.function = function;
    error = failure(LLVMFrontendStatus::NoInnermostLoop,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_NO_INNERMOST_LOOP,
                    "function has no innermost loop", &context);
    return std::nullopt;
  }

  llvm::Loop* selected = nullptr;
  if (options.loopHeader) {
    for (auto* candidate : loops)
      if (loopHeaderName(*function, *candidate) == *options.loopHeader)
        selected = candidate;
    if (!selected) {
      LoopSelection context;
      context.function = function;
      error = failure(LLVMFrontendStatus::NoInnermostLoop,
                      LLVMFrontendDiagnosticCode::LLVM_FRONTEND_NO_INNERMOST_LOOP,
                      "requested loop header is not an innermost loop: " + *options.loopHeader,
                      &context);
      return std::nullopt;
    }
  } else {
    if (loops.size() != 1) {
      LoopSelection context;
      context.function = function;
      error = failure(LLVMFrontendStatus::AmbiguousLoopSelection,
                      LLVMFrontendDiagnosticCode::LLVM_FRONTEND_AMBIGUOUS_LOOP,
                      "function has multiple innermost loops; select --loop-header", &context);
      return std::nullopt;
    }
    selected = loops.front();
  }

  LoopSelection selection;
  selection.function = function;
  selection.loop = selected;
  selection.block = selected->getHeader();
  selection.exit = selected->getExitBlock();
  selection.preheader = selected->getLoopPreheader();
  selection.branch = llvm::dyn_cast<llvm::BranchInst>(selection.block->getTerminator());
  selection.dominatorTree = std::move(dominatorTree);
  selection.loopInfo = std::move(loopInfo);
  return selection;
}

bool canonicalizeLoopEntry(LoopSelection& selection, LLVMFrontendResult& error) {
  if (selection.preheader)
    return true;

  for (auto* block : selection.loop->blocks()) {
    if (block == selection.block)
      continue;
    for (auto* predecessor : llvm::predecessors(block)) {
      if (!selection.loop->contains(predecessor)) {
        error =
            failure(LLVMFrontendStatus::UnsupportedLoopShape,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NON_LINEAR_CFG,
                    "selected natural loop has an outside side entry that cannot be canonicalized",
                    &selection);
        return false;
      }
    }
  }

  auto* preheader = llvm::InsertPreheaderForLoop(selection.loop, selection.dominatorTree.get(),
                                                 selection.loopInfo.get(), nullptr, true);
  if (!preheader) {
    error = failure(LLVMFrontendStatus::UnsupportedLoopShape,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER,
                    "LLVM could not construct a semantics-preserving selected-loop preheader",
                    &selection);
    return false;
  }
  selection.preheader = preheader;
  selection.loopEntryCanonicalized = true;
  return true;
}

bool shapeIsValid(const LoopSelection& selection, LLVMFrontendResult& error) {
  const auto failShape = [&](std::string message) {
    error = failure(LLVMFrontendStatus::UnsupportedLoopShape,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE,
                    std::move(message), &selection);
    return false;
  };
  if (selection.loop->getBlocks().size() != 1)
    return failShape("V0 requires a single-basic-block innermost loop (blocks=" +
                     std::to_string(selection.loop->getBlocks().size()) + ")");
  if (selection.loop->getLoopLatch() != selection.block ||
      selection.loop->getExitingBlock() != selection.block)
    return failShape("V0 requires the loop header to be the single latch and exiting block");
  if (!selection.exit || !selection.branch || !selection.branch->isConditional())
    return failShape("V0 requires one conditional loop terminator and one exit");
  if (selection.branch->getNumSuccessors() != 2)
    return failShape("loop terminator must have exactly two successors");
  unsigned inLoop = 0;
  unsigned outLoop = 0;
  for (const auto* successor : selection.branch->successors()) {
    if (selection.loop->contains(successor))
      ++inLoop;
    else if (successor == selection.exit)
      ++outLoop;
    else
      return failShape("loop terminator has an unexpected successor");
  }
  if (inLoop != 1 || outLoop != 1)
    return failShape("loop terminator must have one backedge and one exit edge");
  return true;
}

bool discoverRecurrences(LoweringState& state, LLVMFrontendResult& error) {
  for (const auto& instruction : *state.selection.block) {
    const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
    if (!phi)
      continue;

    if (!state.selection.preheader) {
      bool hasDataUse = false;
      for (const auto* user : phi->users()) {
        const auto* userInstruction = llvm::dyn_cast<llvm::Instruction>(user);
        if (userInstruction && state.selection.loop->contains(userInstruction) &&
            !state.controlSlice.contains(userInstruction) && !userInstruction->isTerminator() &&
            !ignoredInstruction(*userInstruction)) {
          hasDataUse = true;
          break;
        }
      }
      if (!hasDataUse)
        continue;
    }

    if (phi->getNumIncomingValues() != 2)
      return recurrenceFailure(
          error, LLVMFrontendStatus::UnsupportedRecurrenceShape,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE,
          "canonical recurrence PHI must have exactly two incoming values", state.selection, phi);
    int preheaderIndex = -1;
    int latchIndex = -1;
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      if (phi->getIncomingBlock(index) == state.selection.preheader)
        preheaderIndex = static_cast<int>(index);
      else if (phi->getIncomingBlock(index) == state.selection.block)
        latchIndex = static_cast<int>(index);
      else
        return recurrenceFailure(
            error, LLVMFrontendStatus::UnsupportedRecurrenceShape,
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE,
            "recurrence PHI must have one preheader and one latch incoming", state.selection, phi);
    }
    if (preheaderIndex < 0 || latchIndex < 0)
      return recurrenceFailure(
          error, LLVMFrontendStatus::UnsupportedRecurrenceShape,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE,
          "recurrence PHI is missing a canonical preheader or latch incoming", state.selection,
          phi);
    const auto type = valueType(*phi);
    if (!type)
      return recurrenceFailure(
          error, LLVMFrontendStatus::UnsupportedRecurrenceType,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE,
          "recurrence PHI must have a scalar integer type", state.selection, phi);

    bool dataUse = false;
    for (const auto* user : phi->users()) {
      const auto* userInstruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (!userInstruction)
        continue;
      if (!state.selection.loop->contains(userInstruction))
        return recurrenceFailure(
            error, LLVMFrontendStatus::UnsupportedPhiLiveOutSemantics,
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PHI_LIVEOUT_SEMANTICS,
            "header PHI cannot be used directly outside the loop; use a trivial LCSSA value",
            state.selection, userInstruction);
      if (llvm::isa<llvm::PHINode>(userInstruction))
        return recurrenceFailure(error, LLVMFrontendStatus::UnsupportedPhiToPhiUse,
                                 LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PHI_TO_PHI_USE,
                                 "PHI-to-PHI data recurrence is outside the V0 subset",
                                 state.selection, userInstruction);
      if (userInstruction->isTerminator() || llvm::isa<llvm::ICmpInst>(userInstruction))
        continue;
      if (state.controlSlice.contains(userInstruction))
        continue;
      if (ignoredInstruction(*userInstruction))
        continue;
      if (isMemoryInstruction(*userInstruction))
        return recurrenceFailure(error, LLVMFrontendStatus::UnsupportedMemoryOperation,
                                 LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY,
                                 "memory-carried recurrence is deferred to T018", state.selection,
                                 userInstruction);
      if (!supportedDataInstruction(*userInstruction))
        return recurrenceFailure(error, LLVMFrontendStatus::UnsupportedInstruction,
                                 LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE,
                                 "recurrence PHI data use is outside the scalar integer subset",
                                 state.selection, userInstruction);
      dataUse = true;
    }
    if (!dataUse)
      continue; // A control-only induction PHI remains in LoopControlSlice.

    const auto* initial = phi->getIncomingValue(static_cast<unsigned>(preheaderIndex));
    if (!valueType(*initial))
      return recurrenceFailure(
          error, LLVMFrontendStatus::UnsupportedRecurrenceType,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE,
          "recurrence initial provider has an unsupported type", state.selection, phi);
    if (!constantBits(*initial)) {
      if (llvm::isa<llvm::Constant>(initial))
        return recurrenceFailure(
            error, LLVMFrontendStatus::UnsupportedRecurrenceProvider,
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE,
            "recurrence initial provider must be a scalar literal or loop-external scalar",
            state.selection, phi);
      if (const auto* initialInstruction = llvm::dyn_cast<llvm::Instruction>(initial);
          initialInstruction && state.selection.loop->contains(initialInstruction))
        return recurrenceFailure(
            error, LLVMFrontendStatus::UnsupportedRecurrenceProvider,
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE,
            "recurrence initial provider must be defined outside the selected loop",
            state.selection, phi);
    }

    const auto* backedge = phi->getIncomingValue(static_cast<unsigned>(latchIndex));
    const auto* backedgeInstruction = llvm::dyn_cast<llvm::Instruction>(backedge);
    if (!backedgeInstruction || !state.selection.loop->contains(backedgeInstruction) ||
        llvm::isa<llvm::PHINode>(backedgeInstruction) ||
        !supportedDataInstruction(*backedgeInstruction))
      return recurrenceFailure(
          error, LLVMFrontendStatus::UnsupportedRecurrenceProvider,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_PRODUCER,
          "recurrence latch incoming must be a supported in-loop data instruction", state.selection,
          phi);

    LLVMRecurrenceProvenance descriptor;
    descriptor.id = static_cast<std::uint32_t>(state.provenance.recurrences.size());
    descriptor.phi = valueSummary(*phi);
    descriptor.type = type->toString();
    descriptor.preheader = blockName(*state.selection.function, *state.selection.preheader);
    descriptor.initialValue = valueSummary(*initial);
    descriptor.latch = blockName(*state.selection.function, *state.selection.block);
    descriptor.backedgeValue = valueSummary(*backedge);
    descriptor.distance = 1;
    descriptor.phiValue = phi;
    descriptor.initial = initial;
    descriptor.backedge = backedge;
    state.recurrences.emplace(phi, state.provenance.recurrences.size());
    state.recurrenceBackedges.insert(backedgeInstruction);
    state.provenance.recurrences.push_back(std::move(descriptor));
  }
  return true;
}

bool buildControlSlice(LoweringState& state, LLVMFrontendResult& error) {
  const auto* condition = state.selection.branch->getCondition();
  const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(condition);
  if (!compare) {
    error = failure(LLVMFrontendStatus::DataDependentLoopControl,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DATA_DEPENDENT_CONTROL,
                    "loop terminator condition must be an integer comparison", &state.selection,
                    llvm::dyn_cast<llvm::Instruction>(condition));
    return false;
  }
  std::queue<const llvm::Value*> work;
  work.push(condition);
  while (!work.empty()) {
    const auto* value = work.front();
    work.pop();
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || !state.selection.loop->contains(instruction))
      continue;
    if (!state.controlSlice.insert(instruction).second)
      continue;
    state.provenance.controlSlice.push_back(instruction->getOpcodeName());
    for (const auto& operand : instruction->operands())
      work.push(operand.get());
  }

  for (const auto* instruction : state.controlSlice) {
    if (!llvm::isa<llvm::PHINode>(instruction) && !llvm::isa<llvm::ICmpInst>(instruction) &&
        !supportedDataInstruction(*instruction)) {
      error =
          failure(LLVMFrontendStatus::DataDependentLoopControl,
                  LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DATA_DEPENDENT_CONTROL,
                  "unsupported instruction in loop-control slice", &state.selection, instruction);
      return false;
    }
  }
  return true;
}

bool validateControlDataUses(LoweringState& state, LLVMFrontendResult& error) {
  for (const auto* instruction : state.controlSlice) {
    const bool recurrenceValue =
        state.recurrenceBackedges.contains(instruction) ||
        (llvm::isa<llvm::PHINode>(instruction) &&
         state.recurrences.contains(llvm::cast<llvm::PHINode>(instruction)));
    for (const auto* user : instruction->users()) {
      const auto* userInstruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (!userInstruction || !state.selection.loop->contains(userInstruction) ||
          state.controlSlice.contains(userInstruction) || userInstruction->isTerminator() ||
          ignoredInstruction(*userInstruction))
        continue;
      if (recurrenceValue)
        continue;
      error =
          failure(LLVMFrontendStatus::UnsupportedInductionDataUse,
                  LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INDUCTION_DATA_USE,
                  "loop-control value is used by the data path", &state.selection, userInstruction);
      return false;
    }
  }
  return true;
}

void promoteRecurrenceProducerClosure(LoweringState& state) {
  std::vector<const llvm::Instruction*> work(state.recurrenceBackedges.begin(),
                                             state.recurrenceBackedges.end());
  std::unordered_set<const llvm::Instruction*> visited;
  while (!work.empty()) {
    const auto* instruction = work.back();
    work.pop_back();
    if (!visited.insert(instruction).second)
      continue;
    state.controlSlice.erase(instruction);
    for (const auto& operand : instruction->operands()) {
      const auto* dependency = llvm::dyn_cast<llvm::Instruction>(operand.get());
      if (!dependency || !state.selection.loop->contains(dependency) ||
          llvm::isa<llvm::PHINode>(dependency) || !supportedDataInstruction(*dependency))
        continue;
      work.push_back(dependency);
    }
  }

  state.provenance.controlSlice.clear();
  for (const auto& instruction : *state.selection.block)
    if (state.controlSlice.contains(&instruction))
      state.provenance.controlSlice.push_back(instruction.getOpcodeName());
}

std::string externalName(const llvm::Value& value, std::uint32_t ordinal) {
  if (value.hasName())
    return value.getName().str();
  if (const auto* argument = llvm::dyn_cast<llvm::Argument>(&value))
    return "arg." + std::to_string(argument->getArgNo());
  return "external." + std::to_string(ordinal);
}

bool isTrivialLCSSAPhi(const llvm::PHINode& phi, const llvm::Value& source,
                       const LoopSelection& selection) {
  if (phi.getParent() != selection.exit)
    return false;
  unsigned sourceCount = 0;
  for (unsigned index = 0; index < phi.getNumIncomingValues(); ++index) {
    const auto* incoming = phi.getIncomingValue(index);
    if (incoming == &source) {
      ++sourceCount;
      continue;
    }
    if (!llvm::isa<llvm::UndefValue>(incoming))
      return false;
  }
  return sourceCount == 1;
}

bool hasOutsideUse(const llvm::Value& value, const LoopSelection& selection, bool& badMerge) {
  for (const auto* user : value.users()) {
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
    if (instruction && selection.loop->contains(instruction))
      continue;
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
      if (!isTrivialLCSSAPhi(*phi, value, selection)) {
        badMerge = true;
        return false;
      }
      for (const auto* phiUser : phi->users()) {
        const auto* phiUserInstruction = llvm::dyn_cast<llvm::Instruction>(phiUser);
        if (!phiUserInstruction || !selection.loop->contains(phiUserInstruction))
          return true;
      }
      continue;
    }
    if (instruction && ignoredInstruction(*instruction))
      continue;
    return true;
  }
  return false;
}

std::string sourceLabel(const llvm::Function& function, const llvm::BasicBlock& block,
                        const llvm::Instruction& instruction, std::uint32_t ordinal) {
  std::string label = "llvm." + function.getName().str() + "." + blockName(function, block) + "." +
                      std::to_string(ordinal) + "." + instruction.getOpcodeName();
  if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
    std::string flags;
    if (binary->hasNoSignedWrap())
      flags += "nsw,";
    if (binary->hasNoUnsignedWrap())
      flags += "nuw,";
    if (binary->isExact())
      flags += "exact,";
    if (!flags.empty()) {
      flags.pop_back();
      label += ".flags=" + flags;
    }
  }
  return label;
}

ir::ConstantId getConstant(LoweringState& state, ir::DFGBuilder& builder,
                           const llvm::Constant& constant, const ir::ValueType& type) {
  const auto bits = constantBits(constant);
  if (!bits)
    throw std::invalid_argument("unsupported Generic constant kind");
  const auto key = std::make_tuple(type.kind, type.bitWidth, *bits);
  if (const auto iterator = state.constants.find(key); iterator != state.constants.end())
    return iterator->second;
  const auto id = builder.addConstant(type, *bits);
  state.constants.emplace(key, id);
  return id;
}

ir::ExternalValueId getExternal(LoweringState& state, ir::DFGBuilder& builder,
                                const llvm::Value& value, const ir::ValueType& type) {
  if (const auto iterator = state.externals.find(&value); iterator != state.externals.end())
    return iterator->second;
  auto name = externalName(value, state.nextExternalOrdinal++);
  if (state.externalNames.contains(name))
    name += "." + std::to_string(state.nextExternalOrdinal++);
  state.externalNames.insert(name);
  const auto id = builder.addExternal(name, type);
  state.externals.emplace(&value, id);
  return id;
}

struct IfLoweringState {
  struct GEPAddressNodes {
    ir::NodeId address = 0;
    ir::ValueType type = ir::ValueType::i32();
    std::vector<ir::NodeId> termNodes;
    std::vector<ir::NodeId> sumNodes;
    bool hasConstantSum = false;
  };

  struct SyntheticAddressNode {
    ir::NodeId node = 0;
    const llvm::GetElementPtrInst* gep = nullptr;
    std::string role;
  };

  explicit IfLoweringState(LoopSelection& selected) : selection(selected) {}
  LoopSelection& selection;
  BranchRegion region;
  std::unordered_map<const llvm::Value*, ir::NodeId> nodes;
  std::unordered_map<const llvm::Value*, ir::ExternalValueId> externals;
  std::unordered_map<const llvm::Value*, ir::ConstantId> constants;
  std::map<std::pair<std::uint16_t, std::uint64_t>, ir::ConstantId> addressConstants;
  std::set<std::string> externalNames;
  std::set<std::string> liveOutNames;
  std::uint32_t nextExternalOrdinal = 0;
  std::unordered_set<const llvm::Instruction*> terminationSlice;
  std::unordered_set<const llvm::Instruction*> predicateSlice;
  std::unordered_map<const llvm::PHINode*, ir::NodeId> selects;
  std::vector<const llvm::PHINode*> selectOrder;
  std::unordered_map<const llvm::PHINode*, std::size_t> recurrences;
  std::unordered_set<const llvm::Instruction*> recurrenceBackedges;
  std::unordered_map<const llvm::GetElementPtrInst*, GEPAddressNodes> gepAddressNodes;
  std::vector<SyntheticAddressNode> syntheticAddressNodes;
  LLVMFrontendProvenance provenance;
};

bool discoverIfRecurrences(IfLoweringState& state, const llvm::Module& module,
                           LLVMFrontendResult& error) {
  auto* preheader = state.selection.loop->getLoopPreheader();
  auto* latch = state.selection.loop->getLoopLatch();
  if (!preheader || !latch)
    return true;
  for (const auto& instruction : *state.selection.block) {
    const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
    if (!phi || phi->getNumIncomingValues() != 2)
      continue;
    const int initialIndex = phi->getBasicBlockIndex(preheader);
    const int backedgeIndex = phi->getBasicBlockIndex(latch);
    if (initialIndex < 0 || backedgeIndex < 0)
      continue;
    const auto* backedge = llvm::dyn_cast<llvm::Instruction>(
        phi->getIncomingValue(static_cast<unsigned>(backedgeIndex)));
    const auto* backedgePhi = llvm::dyn_cast_or_null<llvm::PHINode>(backedge);
    const bool conditionalSelectBackedge = backedgePhi && state.region.branch &&
                                           backedgePhi->getParent() == state.region.mergeBlock &&
                                           backedgePhi->getNumIncomingValues() == 2;
    const bool pointerRecurrence =
        phi->getType()->isPointerTy() &&
        llvm::isa_and_nonnull<llvm::GetElementPtrInst, llvm::SelectInst, llvm::PHINode>(backedge);
    if (backedgePhi && !conditionalSelectBackedge) {
      recurrenceFailure(
          error, LLVMFrontendStatus::ConditionalRecurrenceUnsupported,
          LLVMFrontendDiagnosticCode::LLVM_FRONTEND_CONDITIONAL_RECURRENCE_UNSUPPORTED,
          "conditional recurrence backedge is not a supported structured Select", state.selection,
          phi);
      return false;
    }
    if (!backedge || !state.selection.loop->contains(backedge) ||
        (!conditionalSelectBackedge && !pointerRecurrence &&
         !supportedRecurrenceProducer(*backedge)))
      continue;
    bool dataUse = false;
    for (const auto* user : phi->users()) {
      const auto* userInstruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (userInstruction && state.selection.loop->contains(userInstruction) &&
          !state.terminationSlice.contains(userInstruction) && !userInstruction->isTerminator() &&
          !ignoredInstruction(*userInstruction)) {
        dataUse = true;
        break;
      }
    }
    if (!dataUse)
      continue;
    const auto type =
        phi->getType()->isPointerTy() ? addressValueType(module, *phi) : valueType(*phi);
    if (!type)
      continue;
    LLVMRecurrenceProvenance descriptor;
    descriptor.id = static_cast<std::uint32_t>(state.provenance.recurrences.size());
    descriptor.phi = valueSummary(*phi);
    descriptor.type = type->toString();
    descriptor.preheader = blockName(*state.selection.function, *preheader);
    descriptor.initialValue =
        valueSummary(*phi->getIncomingValue(static_cast<unsigned>(initialIndex)));
    descriptor.latch = blockName(*state.selection.function, *latch);
    descriptor.backedgeValue = valueSummary(*backedge);
    descriptor.distance = 1;
    descriptor.phiValue = phi;
    descriptor.initial = phi->getIncomingValue(static_cast<unsigned>(initialIndex));
    descriptor.backedge = backedge;
    state.recurrences.emplace(phi, state.provenance.recurrences.size());
    state.recurrenceBackedges.insert(backedge);
    state.provenance.recurrences.push_back(std::move(descriptor));
  }
  return true;
}

void promoteIfRecurrenceProducerClosure(IfLoweringState& state) {
  std::vector<const llvm::Instruction*> work(state.recurrenceBackedges.begin(),
                                             state.recurrenceBackedges.end());
  std::unordered_set<const llvm::Instruction*> visited;
  while (!work.empty()) {
    const auto* instruction = work.back();
    work.pop_back();
    if (!visited.insert(instruction).second)
      continue;
    state.terminationSlice.erase(instruction);
    for (const auto& operand : instruction->operands()) {
      const auto* dependency = llvm::dyn_cast<llvm::Instruction>(operand.get());
      const auto* dependencyPhi = llvm::dyn_cast_or_null<llvm::PHINode>(dependency);
      const bool mergePhi = dependencyPhi && dependencyPhi->getParent() != state.selection.block;
      const bool pointerExpression =
          dependency && dependency->getType()->isPointerTy() &&
          llvm::isa<llvm::GetElementPtrInst, llvm::SelectInst, llvm::PHINode>(dependency);
      if (!dependency || !state.selection.loop->contains(dependency) ||
          (!mergePhi && !pointerExpression && !supportedRecurrenceProducer(*dependency)))
        continue;
      work.push_back(dependency);
    }
  }
}

std::string blockName(const llvm::Function& function, const llvm::BasicBlock* block) {
  return block ? blockName(function, *block) : "";
}

void collectSlice(const llvm::Value* root, const llvm::Loop& loop,
                  std::unordered_set<const llvm::Instruction*>& result) {
  std::vector<const llvm::Value*> work{root};
  while (!work.empty()) {
    const auto* value = work.back();
    work.pop_back();
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || !loop.contains(instruction) || !result.insert(instruction).second)
      continue;
    for (const auto& operand : instruction->operands())
      work.push_back(operand.get());
  }
}

std::optional<BranchRegion> discoverBranchRegion(LoopSelection& selection,
                                                 LLVMFrontendResult& error) {
  std::vector<llvm::BranchInst*> branches;
  for (auto* block : selection.loop->getBlocks()) {
    auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (branch && branch->isConditional() && branch->getNumSuccessors() == 2) {
      bool bothInLoop = selection.loop->contains(branch->getSuccessor(0)) &&
                        selection.loop->contains(branch->getSuccessor(1));
      if (bothInLoop)
        branches.push_back(branch);
    }
  }
  if (branches.size() > 1) {
    error = failure(LLVMFrontendStatus::MultipleInternalBranches,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MULTIPLE_INTERNAL_BRANCHES,
                    "V0 supports at most one internal conditional branch", &selection,
                    branches.front());
    return std::nullopt;
  }
  if (branches.empty())
    return std::nullopt;

  auto* branch = branches.front();
  const auto* condition = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
  if (!condition) {
    error = failure(LLVMFrontendStatus::UnsupportedBranchCondition,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
                    "internal branch condition must be an ICmp", &selection, branch);
    return std::nullopt;
  }
  llvm::PostDominatorTree postDominators(*selection.function);
  auto* merge =
      postDominators.findNearestCommonDominator(branch->getSuccessor(0), branch->getSuccessor(1));
  if (!merge || !selection.loop->contains(merge) || merge == branch->getParent()) {
    error = failure(LLVMFrontendStatus::BranchNoUniqueMerge,
                    LLVMFrontendDiagnosticCode::LLVM_FRONTEND_BRANCH_NO_UNIQUE_MERGE,
                    "internal branch has no unique in-loop merge", &selection, branch);
    return std::nullopt;
  }
  for (auto* block : selection.loop->getBlocks()) {
    auto* terminator = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!terminator || !terminator->isConditional() || terminator == branch)
      continue;
    bool exitsLoop = false;
    for (auto* successor : terminator->successors())
      exitsLoop |= !selection.loop->contains(successor);
    if (!exitsLoop) {
      error = failure(LLVMFrontendStatus::NestedPredicationUnsupported,
                      LLVMFrontendDiagnosticCode::LLVM_FRONTEND_NESTED_PREDICATION_UNSUPPORTED,
                      "nested or additional internal conditional branch is outside V0", &selection,
                      terminator);
      return std::nullopt;
    }
  }
  BranchRegion result;
  result.branch = branch;
  result.conditionBlock = branch->getParent();
  result.trueBlock = branch->getSuccessor(0);
  result.falseBlock = branch->getSuccessor(1);
  result.mergeBlock = merge;
  result.condition = condition;
  return result;
}

void collectTerminationSlice(IfLoweringState& state) {
  for (auto* block : state.selection.loop->getBlocks()) {
    auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isConditional())
      continue;
    bool exitsLoop = false;
    for (auto* successor : branch->successors())
      exitsLoop |= !state.selection.loop->contains(successor);
    if (exitsLoop)
      collectSlice(branch->getCondition(), *state.selection.loop, state.terminationSlice);
  }
  if (state.region.condition)
    collectSlice(state.region.condition, *state.selection.loop, state.predicateSlice);
}

std::string externalNameForIf(const llvm::Value& value, std::uint32_t ordinal) {
  if (value.hasName())
    return value.getName().str();
  if (const auto* argument = llvm::dyn_cast<llvm::Argument>(&value))
    return "arg." + std::to_string(argument->getArgNo());
  return "external." + std::to_string(ordinal);
}

ir::ExternalValueId getIfExternal(IfLoweringState& state, ir::DFGBuilder& builder,
                                  const llvm::Value& value, const ir::ValueType& type) {
  if (const auto iterator = state.externals.find(&value); iterator != state.externals.end())
    return iterator->second;
  auto name = externalNameForIf(value, state.nextExternalOrdinal++);
  while (state.externalNames.contains(name))
    name += "." + std::to_string(state.nextExternalOrdinal++);
  state.externalNames.insert(name);
  const auto id = builder.addExternal(name, type);
  state.externals.emplace(&value, id);
  return id;
}

ir::ConstantId getIfConstant(IfLoweringState& state, ir::DFGBuilder& builder,
                             const llvm::Constant& constant, const ir::ValueType& type) {
  if (const auto iterator = state.constants.find(&constant); iterator != state.constants.end())
    return iterator->second;
  const auto bits = constantBits(constant);
  if (!bits)
    throw std::invalid_argument("unsupported Generic constant kind");
  const auto id = builder.addConstant(type, *bits);
  state.constants.emplace(&constant, id);
  return id;
}

ir::ConstantId getAddressConstant(IfLoweringState& state, ir::DFGBuilder& builder,
                                  const ir::ValueType& type, std::int64_t value) {
  auto bits = static_cast<std::uint64_t>(value);
  if (type.bitWidth < 64)
    bits &= (std::uint64_t{1} << type.bitWidth) - 1;
  const auto key = std::pair{type.bitWidth, bits};
  if (const auto iterator = state.addressConstants.find(key);
      iterator != state.addressConstants.end())
    return iterator->second;
  const auto id = builder.addConstant(type, bits);
  state.addressConstants.emplace(key, id);
  return id;
}

bool valueIsInBranch(const llvm::Value& value, const BranchRegion& region, const llvm::Loop& loop) {
  const auto* instruction = llvm::dyn_cast<llvm::Instruction>(&value);
  if (!instruction || !loop.contains(instruction))
    return false;
  const auto* block = instruction->getParent();
  return block == region.trueBlock || block == region.falseBlock;
}

bool isConditionalStore(const llvm::StoreInst& store, const BranchRegion& region) {
  return store.getParent() == region.trueBlock || store.getParent() == region.falseBlock;
}

std::optional<std::uint64_t> inferStaticTripCount(LoopSelection& selection) {
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(*selection.function);
  llvm::ScalarEvolution scalarEvolution(*selection.function, libraryInfo, assumptions,
                                        *selection.dominatorTree, *selection.loopInfo);
  const auto tripCount = scalarEvolution.getSmallConstantTripCount(selection.loop);
  if (tripCount == 0)
    return std::nullopt;
  return tripCount;
}

std::optional<ir::ValueType> addressValueType(const llvm::Module& module,
                                              const llvm::Value& address) {
  if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&address)) {
    for (const auto& index : gep->indices())
      if (!llvm::isa<llvm::ConstantInt>(index))
        return valueType(*index);
  }
  if (const auto* pointer = llvm::dyn_cast<llvm::PointerType>(address.getType())) {
    const auto bits = module.getDataLayout().getPointerSizeInBits(pointer->getAddressSpace());
    if (bits > 0 && bits <= 64)
      return ir::ValueType::integer(static_cast<std::uint16_t>(bits));
    return std::nullopt;
  }
  return valueType(address);
}

std::optional<std::int64_t> constantGEPOffsetUnits(const llvm::Module& module,
                                                   const llvm::GetElementPtrInst& gep,
                                                   std::uint32_t addressUnitBytes) {
  if (addressUnitBytes == 0)
    return std::nullopt;
  const auto& dataLayout = module.getDataLayout();
  const auto bitWidth = dataLayout.getIndexTypeSizeInBits(gep.getType());
  llvm::MapVector<llvm::Value*, llvm::APInt> offsets;
  llvm::APInt constant(bitWidth, 0, true);
  if (!gep.collectOffset(dataLayout, bitWidth, offsets, constant) || !offsets.empty() ||
      constant.getMinSignedBits() > 64)
    return std::nullopt;
  const auto bytes = constant.getSExtValue();
  if (bytes % addressUnitBytes != 0)
    return std::nullopt;
  return bytes / addressUnitBytes;
}

std::optional<ir::ValueType> pointerTokenType(const llvm::Module& module,
                                              const LoopSelection& selection,
                                              const llvm::Value& pointer) {
  std::optional<ir::ValueType> result;
  for (const auto* block : selection.loop->getBlocks()) {
    for (const auto& instruction : *block) {
      const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      const auto* address = load    ? load->getPointerOperand()
                            : store ? store->getPointerOperand()
                                    : nullptr;
      const auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(address);
      if (!gep || llvm::getUnderlyingObject(gep) != &pointer)
        continue;
      const auto candidate = addressValueType(module, *gep);
      if (!candidate || (result && *result != *candidate))
        return std::nullopt;
      result = candidate;
    }
  }
  return result ? result : addressValueType(module, pointer);
}

LLVMFrontendResult lowerStructuredLoop(llvm::Module& module, const LLVMFrontendOptions& options,
                                       LoopSelection& selection,
                                       std::optional<BranchRegion> discoveredRegion) {
  LLVMFrontendResult error;
  IfLoweringState state{selection};
  if (discoveredRegion)
    state.region = *discoveredRegion;
  collectTerminationSlice(state);

  if (!discoverIfRecurrences(state, module, error))
    return error;
  promoteIfRecurrenceProducerClosure(state);

  const auto memoryAnalysis = analyzeMemoryDependences(
      *selection.loop, *selection.dominatorTree, *selection.loopInfo, options.addressUnitBytes);
  if (!memoryAnalysis.ok()) {
    LLVMFrontendStatus status = LLVMFrontendStatus::UnsupportedMemoryOperation;
    LLVMFrontendDiagnosticCode code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY;
    switch (memoryAnalysis.status) {
    case LLVMMemoryAnalysisStatus::UnsupportedAccessType:
      status = LLVMFrontendStatus::UnsupportedMemoryType;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE;
      break;
    case LLVMMemoryAnalysisStatus::UnsupportedAddressSpace:
      status = LLVMFrontendStatus::UnsupportedMemoryAddressSpace;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_ADDRESS_SPACE;
      break;
    case LLVMMemoryAnalysisStatus::UnsupportedAlignment:
      status = LLVMFrontendStatus::UnsupportedMemoryAlignment;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_ALIGNMENT;
      break;
    case LLVMMemoryAnalysisStatus::UnsupportedPointerBase:
      status = LLVMFrontendStatus::UnsupportedPointerBase;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE;
      break;
    case LLVMMemoryAnalysisStatus::UnsupportedNonAffineAddress:
      status = LLVMFrontendStatus::UnsupportedNonAffineAddress;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS;
      break;
    case LLVMMemoryAnalysisStatus::UnsupportedPathSensitiveOrder:
      status = LLVMFrontendStatus::UnsupportedPathSensitiveMemoryOrder;
      code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PATH_SENSITIVE_MEMORY_ORDER;
      break;
    case LLVMMemoryAnalysisStatus::Success:
    case LLVMMemoryAnalysisStatus::UnsupportedVolatileOrAtomic:
    case LLVMMemoryAnalysisStatus::InternalError:
      break;
    }
    return failure(status, code, memoryAnalysis.message, &selection);
  }
  std::unordered_map<const llvm::Instruction*, const LLVMMemoryAccessDescriptor*> memoryAccesses;
  for (const auto& access : memoryAnalysis.accesses)
    memoryAccesses.emplace(access.instruction, &access);

  std::vector<const llvm::StoreInst*> stores;
  for (auto* block : selection.loop->getBlocks())
    for (const auto& instruction : *block)
      if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        stores.push_back(store);
  bool hasTrueStore = false;
  bool hasFalseStore = false;
  if (discoveredRegion) {
    hasTrueStore = std::ranges::any_of(
        stores, [&](const auto* store) { return store->getParent() == state.region.trueBlock; });
    hasFalseStore = std::ranges::any_of(
        stores, [&](const auto* store) { return store->getParent() == state.region.falseBlock; });
    if (hasTrueStore && hasFalseStore)
      return failure(LLVMFrontendStatus::UnsupportedIfSideEffect,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT,
                     "Stores in both branch arms require two predicate polarities", &selection,
                     stores.front());
  }
  const bool storePredicateComplemented = discoveredRegion && hasFalseStore && !hasTrueStore;
  if (storePredicateComplemented) {
    const auto predicate = icmpPredicate(*state.region.condition);
    if (!predicate || !complementPredicate(*predicate))
      return failure(LLVMFrontendStatus::UnsupportedPredicateComplement,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PREDICATE_COMPLEMENT,
                     "cannot complement the false-arm Store predicate", &selection, stores.front());
    std::swap(state.region.trueBlock, state.region.falseBlock);
  }
  if (discoveredRegion) {
    state.provenance.ifConversions.push_back(
        {0,
         blockName(*selection.function, state.region.conditionBlock),
         blockName(*selection.function, state.region.trueBlock),
         blockName(*selection.function, state.region.falseBlock),
         blockName(*selection.function, state.region.mergeBlock),
         valueSummary(*state.region.condition),
         false,
         0,
         {},
         {},
         {},
         state.region.branch,
         state.region.condition});
    state.provenance.ifConversions.front().predicateComplemented = storePredicateComplemented;
  }

  // Build control-merge Select nodes before ordinary nodes so PHI users can resolve to them.
  ir::DFGBuilder builder(selection.function->getName().str());
  std::vector<const llvm::PHINode*> mergePhis;
  if (discoveredRegion) {
    for (const auto& instruction : *state.region.mergeBlock) {
      const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
      if (!phi)
        continue;
      if (phi->getType()->isIntegerTy(1))
        return failure(LLVMFrontendStatus::UnsupportedPredicateMerge,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PREDICATE_MERGE,
                       "predicate-valued merge is outside the V0 subset", &selection, phi);
      int trueIndex = phi->getBasicBlockIndex(state.region.trueBlock);
      int falseIndex = phi->getBasicBlockIndex(state.region.falseBlock);
      if (falseIndex < 0 && state.region.falseBlock == state.region.mergeBlock)
        falseIndex = phi->getBasicBlockIndex(state.region.conditionBlock);
      if (trueIndex < 0 || falseIndex < 0 || phi->getNumIncomingValues() != 2)
        return failure(LLVMFrontendStatus::UnsupportedControlMerge,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_CONTROL_MERGE,
                       "merge PHI must have exactly true and false incoming values", &selection,
                       phi);
      const auto type =
          phi->getType()->isPointerTy() ? addressValueType(module, *phi) : valueType(*phi);
      if (!type)
        return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                       "control merge PHI must have a supported scalar or address type", &selection,
                       phi);
      const auto select =
          builder.addNode(ir::Opcode::Select, {ir::ValueType::predicate(), *type, *type}, *type);
      state.selects.emplace(phi, select);
      state.selectOrder.push_back(phi);
      mergePhis.push_back(phi);
    }
  }

  std::vector<llvm::BasicBlock*> blocks;
  if (selection.linearRegion)
    blocks = selection.linearRegion->orderedBlocks;
  else
    blocks.assign(selection.loop->getBlocks().begin(), selection.loop->getBlocks().end());

  auto shouldSkip = [&](const llvm::Instruction& instruction) {
    if (ignoredInstruction(instruction) || instruction.isTerminator() ||
        llvm::isa<llvm::PHINode>(instruction))
      return true;
    if (state.terminationSlice.contains(&instruction) &&
        !state.recurrenceBackedges.contains(&instruction) &&
        !state.predicateSlice.contains(&instruction))
      return true;
    return false;
  };

  std::unordered_set<const llvm::GetElementPtrInst*> addressChainGEPs;
  struct PointerRecurrenceUpdate {
    ir::ValueType type;
    std::int64_t step = 0;
  };
  std::unordered_map<const llvm::GetElementPtrInst*, PointerRecurrenceUpdate> pointerGEPUpdates;
  for (const auto& access : memoryAnalysis.accesses) {
    const llvm::Value* cursor = access.address;
    while (const auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(cursor)) {
      addressChainGEPs.insert(gep);
      cursor = gep->getPointerOperand();
      while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(cursor)) {
        if (!llvm::isa<llvm::BitCastInst, llvm::AddrSpaceCastInst>(cast))
          break;
        cursor = cast->getOperand(0);
      }
    }
    if (access.pointerBackedge) {
      const auto type = addressValueType(module, *access.pointerPhi);
      if (!type)
        return failure(LLVMFrontendStatus::UnsupportedPointerBase,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE,
                       "pointer recurrence width is outside the Generic address contract",
                       &selection, access.pointerPhi);
      const auto [iterator, inserted] = pointerGEPUpdates.emplace(
          access.pointerBackedge, PointerRecurrenceUpdate{*type, access.pointerStepWords});
      if (!inserted &&
          (iterator->second.type != *type || iterator->second.step != access.pointerStepWords))
        return failure(LLVMFrontendStatus::UnsupportedNonAffineAddress,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS,
                       "pointer recurrence is used with incompatible Generic address units",
                       &selection, access.pointerBackedge);
    }
  }

  for (auto* block : selection.loop->getBlocks()) {
    for (const auto& instruction : *block) {
      const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
      if (!gep || addressChainGEPs.contains(gep) || pointerGEPUpdates.contains(gep))
        continue;
      const bool pointerDataUse = std::ranges::any_of(gep->users(), [](const auto* user) {
        return llvm::isa<llvm::SelectInst, llvm::ICmpInst, llvm::PHINode, llvm::GetElementPtrInst>(
            user);
      });
      if (!pointerDataUse)
        continue;
      const auto type = addressValueType(module, *gep);
      const auto offset = constantGEPOffsetUnits(module, *gep, options.addressUnitBytes);
      if (!type || !offset)
        return failure(LLVMFrontendStatus::UnsupportedNonAffineAddress,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS,
                       "pointer SSA GEP requires one constant DataLayout offset", &selection, gep);
      pointerGEPUpdates.emplace(gep, PointerRecurrenceUpdate{*type, *offset});
    }
  }

  for (auto* block : blocks) {
    for (const auto& instruction : *block) {
      if (shouldSkip(instruction))
        continue;
      if (llvm::isa<llvm::StoreInst>(instruction))
        continue;
      if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
        if (const auto update = pointerGEPUpdates.find(gep); update != pointerGEPUpdates.end()) {
          state.nodes.emplace(gep, builder.addNode(ir::Opcode::Add,
                                                   {update->second.type, update->second.type},
                                                   update->second.type));
          continue;
        }
        const auto access = std::ranges::find_if(
            memoryAnalysis.accesses, [&](const auto& item) { return item.address == gep; });
        if (access == memoryAnalysis.accesses.end()) {
          if (addressChainGEPs.contains(gep))
            continue;
          return failure(LLVMFrontendStatus::UnsupportedNonAffineAddress,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS,
                         "GEP is not the address of a supported memory operation", &selection, gep);
        }
        IfLoweringState::GEPAddressNodes addressNodes;
        const auto addressType = addressValueType(module, *gep);
        if (!addressType)
          return failure(LLVMFrontendStatus::UnsupportedPointerBase,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE,
                         "GEP pointer width is outside the Generic scalar address contract",
                         &selection, gep);
        addressNodes.type = *addressType;
        addressNodes.address =
            builder.addNode(ir::Opcode::Add, {*addressType, *addressType}, *addressType);
        state.nodes.emplace(gep, addressNodes.address);
        for (std::size_t index = 0; index < access->dynamicTerms.size(); ++index) {
          const auto term =
              builder.addNode(ir::Opcode::Mul, {*addressType, *addressType}, *addressType);
          addressNodes.termNodes.push_back(term);
          state.syntheticAddressNodes.push_back({term, gep, "GEP_TERM_SCALE"});
        }
        const auto termSums = access->dynamicTerms.empty() ? 0 : access->dynamicTerms.size() - 1;
        const auto sumCount =
            termSums +
            (!access->dynamicTerms.empty() && access->gepConstantOffsetWords != 0 ? 1 : 0);
        addressNodes.hasConstantSum = sumCount > termSums;
        for (std::size_t index = 0; index < sumCount; ++index) {
          const auto sum =
              builder.addNode(ir::Opcode::Add, {*addressType, *addressType}, *addressType);
          addressNodes.sumNodes.push_back(sum);
          state.syntheticAddressNodes.push_back(
              {sum, gep,
               addressNodes.hasConstantSum && index + 1 == sumCount ? "GEP_OFFSET"
                                                                    : "GEP_TERM_ADD"});
        }
        state.gepAddressNodes.emplace(gep, addressNodes);
        continue;
      }
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (discoveredRegion && valueIsInBranch(*load, state.region, *selection.loop))
          return failure(LLVMFrontendStatus::PredicatedLoadUnsupported,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PREDICATED_LOAD_UNSUPPORTED,
                         "branch-local conditional Load has no V0 suppression mechanism",
                         &selection, load);
        const auto access = std::ranges::find_if(
            memoryAnalysis.accesses, [&](const auto& item) { return item.instruction == load; });
        const auto loadType = load->getType()->isPointerTy()
                                  ? pointerTokenType(module, selection, *load)
                                  : valueType(*load);
        if (access == memoryAnalysis.accesses.end() || !loadType)
          return failure(LLVMFrontendStatus::UnsupportedMemoryType,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE,
                         "Load type is outside the typed scalar memory contract", &selection, load);
        state.nodes.emplace(
            load, builder.addNode(
                      ir::Opcode::Load, {*addressValueType(module, *load->getPointerOperand())},
                      *loadType, std::nullopt,
                      ir::MemoryOpInfo{access->accessWidthBits, false, access->alignmentBytes}));
        continue;
      }
      if (isMemoryInstruction(instruction)) {
        return failure(LLVMFrontendStatus::UnsupportedMemoryOperation,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY,
                       "memory instruction is outside the T018 V0 subset", &selection,
                       &instruction);
      }
      if (llvm::isa<llvm::CallBase>(&instruction) && !customOperationKey(instruction))
        return failure(LLVMFrontendStatus::UnsupportedIfSideEffect,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT,
                       "calls and side-effecting intrinsics are outside T017 V0", &selection,
                       &instruction);
      const bool safeToSpeculate = llvm::isa<llvm::PtrToIntInst>(instruction) ||
                                   llvm::isSafeToSpeculativelyExecute(&instruction);
      if (!isPureInstruction(instruction) ||
          (discoveredRegion && !state.predicateSlice.contains(&instruction) &&
           valueIsInBranch(instruction, state.region, *selection.loop) && !safeToSpeculate)) {
        return failure(LLVMFrontendStatus::UnsafeSpeculation,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSAFE_SPECULATION,
                       "branch-local instruction is not safe to speculate", &selection,
                       &instruction);
      }
      const auto type = instruction.getType()->isPointerTy() ? addressValueType(module, instruction)
                                                             : valueType(instruction);
      if (!type && !llvm::isa<llvm::ICmpInst>(instruction))
        return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                       "if-converted instruction must produce a supported scalar value", &selection,
                       &instruction);
      if (const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
        const auto predicate = icmpPredicate(*compare);
        if (!predicate)
          return failure(LLVMFrontendStatus::UnsupportedBranchCondition,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
                         "ICmp predicate is outside the Generic subset", &selection, compare);
        const auto lhsType = compare->getOperand(0)->getType()->isPointerTy()
                                 ? addressValueType(module, *compare->getOperand(0))
                                 : valueType(*compare->getOperand(0));
        const auto rhsType = compare->getOperand(1)->getType()->isPointerTy()
                                 ? addressValueType(module, *compare->getOperand(1))
                                 : valueType(*compare->getOperand(1));
        if (!lhsType || !rhsType || *lhsType != *rhsType ||
            lhsType->kind == ir::ValueKind::Predicate)
          return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                         "ICmp operands must have the same supported integer type", &selection,
                         compare);
        auto normalizedPredicate = *predicate;
        if (discoveredRegion && compare == state.region.condition && storePredicateComplemented)
          normalizedPredicate = *complementPredicate(*predicate);
        state.nodes.emplace(&instruction,
                            builder.addNode(ir::Opcode::ICmp, {*lhsType, *rhsType},
                                            ir::ValueType::predicate(), normalizedPredicate));
      } else if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
        const auto selectedType = select->getTrueValue()->getType()->isPointerTy()
                                      ? addressValueType(module, *select->getTrueValue())
                                      : valueType(*select->getTrueValue());
        const auto falseType = select->getFalseValue()->getType()->isPointerTy()
                                   ? addressValueType(module, *select->getFalseValue())
                                   : valueType(*select->getFalseValue());
        if (!selectedType || !falseType || *selectedType != *falseType)
          return failure(LLVMFrontendStatus::UnsupportedControlMerge,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_CONTROL_MERGE,
                         "LLVM Select true and false values must have the same supported type",
                         &selection, select);
        state.nodes.emplace(&instruction, builder.addNode(ir::Opcode::Select,
                                                          {ir::ValueType::predicate(),
                                                           *selectedType, *selectedType},
                                                          *selectedType));
      } else {
        const auto mapped = opcode(instruction);
        const auto custom = customOperationKey(instruction);
        if (!mapped && !custom)
          return failure(LLVMFrontendStatus::UnsupportedInstruction,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE,
                         "instruction is outside the T017 scalar subset", &selection, &instruction);
        std::vector<ir::ValueType> operandTypes;
        for (const auto* operand : semanticOperands(instruction)) {
          const auto operandType = operand->getType()->isPointerTy()
                                       ? addressValueType(module, *operand)
                                       : valueType(*operand);
          if (!operandType)
            return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                           LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                           "arithmetic operand must be an integer scalar", &selection,
                           &instruction);
          operandTypes.push_back(*operandType);
        }
        state.nodes.emplace(
            &instruction, mapped ? builder.addNode(*mapped, std::move(operandTypes), *type)
                                 : builder.addCustomNode(*custom, std::move(operandTypes), *type));
      }
    }
  }

  auto provider = [&](const llvm::Value& value, ir::NodeId destination, std::uint32_t operand,
                      bool predicate) -> std::optional<ir::EdgeId> {
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&value)) {
      if (!selection.loop->contains(phi)) {
        const auto type = valueType(value);
        if (!type)
          return std::nullopt;
        builder.bindExternal(destination, operand, getIfExternal(state, builder, value, *type));
        return std::nullopt;
      }
      const auto select = state.selects.find(phi);
      if (select != state.selects.end()) {
        if (predicate)
          return std::nullopt;
        return builder.addDataEdge(select->second, destination, operand);
      }
      const auto recurrence = state.recurrences.find(phi);
      if (recurrence == state.recurrences.end() || predicate)
        return std::nullopt;
      auto& descriptor = state.provenance.recurrences[recurrence->second];
      std::optional<ir::NodeId> source;
      if (const auto node = state.nodes.find(descriptor.backedge); node != state.nodes.end())
        source = node->second;
      else if (const auto* mergePhi = llvm::dyn_cast<llvm::PHINode>(descriptor.backedge)) {
        if (const auto select = state.selects.find(mergePhi); select != state.selects.end())
          source = select->second;
      }
      if (!source)
        return std::nullopt;
      ir::RecurrenceBoundary boundary;
      const auto type = descriptor.initial->getType()->isPointerTy()
                            ? addressValueType(module, *descriptor.initial)
                            : valueType(*descriptor.initial);
      if (!type)
        return std::nullopt;
      if (const auto* constant = llvm::dyn_cast<llvm::Constant>(descriptor.initial);
          constant && constantBits(*constant))
        boundary.values.push_back(
            {0, ir::ConstantRef{getIfConstant(state, builder, *constant, *type)}});
      else
        boundary.values.push_back(
            {0, ir::ExternalValueRef{getIfExternal(state, builder, *descriptor.initial, *type)}});
      const auto edge = builder.addDataEdge(*source, destination, operand, 1, boundary);
      descriptor.uses.push_back(
          {valueSummary(*llvm::cast<llvm::Instruction>(&value)), operand, destination, edge});
      return edge;
    }
    if (const auto node = state.nodes.find(&value); node != state.nodes.end()) {
      const auto edge = predicate ? builder.addPredicateEdge(node->second, destination, operand)
                                  : builder.addDataEdge(node->second, destination, operand);
      return edge;
    }
    if (const auto* constant = llvm::dyn_cast<llvm::Constant>(&value);
        constant && constantBits(*constant)) {
      const auto type = valueType(value);
      if (!type)
        return std::nullopt;
      builder.bindConstant(destination, operand, getIfConstant(state, builder, *constant, *type));
      return std::nullopt;
    }
    if (value.getType()->isPointerTy()) {
      if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(&value);
          instruction && selection.loop->contains(instruction))
        return std::nullopt;
      builder.bindExternal(destination, operand,
                           getIfExternal(state, builder, value, *addressValueType(module, value)));
      return std::nullopt;
    }
    const auto type = valueType(value);
    if (!type)
      return std::nullopt;
    builder.bindExternal(destination, operand, getIfExternal(state, builder, value, *type));
    return std::nullopt;
  };

  // Wire Select nodes representing merge PHIs and direct LLVM Select nodes.
  for (const auto* phi : state.selectOrder) {
    const auto selectNode = state.selects.at(phi);
    const int trueIndex = phi->getBasicBlockIndex(state.region.trueBlock);
    const int falseIndex = phi->getBasicBlockIndex(state.region.falseBlock) >= 0
                               ? phi->getBasicBlockIndex(state.region.falseBlock)
                               : phi->getBasicBlockIndex(state.region.conditionBlock);
    const auto predicateNode = state.nodes.find(state.region.condition);
    if (predicateNode == state.nodes.end())
      return failure(LLVMFrontendStatus::UnsupportedBranchCondition,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
                     "branch predicate was not lowered", &selection, state.region.branch);
    const auto predicateEdge = builder.addPredicateEdge(predicateNode->second, selectNode, 0);
    const auto trueEdge = provider(*phi->getIncomingValue(trueIndex), selectNode, 1, false);
    const auto falseEdge = provider(*phi->getIncomingValue(falseIndex), selectNode, 2, false);
    static_cast<void>(trueEdge);
    static_cast<void>(falseEdge);
    auto& plan = state.provenance.ifConversions.front();
    plan.predicateNode = predicateNode->second;
    plan.selects.push_back({valueSummary(*phi), selectNode,
                            valueSummary(*phi->getIncomingValue(trueIndex)),
                            valueSummary(*phi->getIncomingValue(falseIndex)), phi, nullptr,
                            phi->getIncomingValue(trueIndex), phi->getIncomingValue(falseIndex)});
    plan.predicateEdges.push_back(predicateEdge);
  }

  for (auto* block : blocks) {
    for (const auto& instruction : *block) {
      const auto destination = state.nodes.find(&instruction);
      if (destination == state.nodes.end())
        continue;
      if (llvm::isa<llvm::StoreInst>(instruction))
        continue;
      if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
        if (const auto update = pointerGEPUpdates.find(gep); update != pointerGEPUpdates.end()) {
          provider(*gep->getPointerOperand(), destination->second, 0, false);
          builder.bindConstant(
              destination->second, 1,
              getAddressConstant(state, builder, update->second.type, update->second.step));
          continue;
        }
        const auto access = std::ranges::find_if(
            memoryAnalysis.accesses, [&](const auto& item) { return item.address == gep; });
        if (access == memoryAnalysis.accesses.end())
          return failure(LLVMFrontendStatus::UnsupportedNonAffineAddress,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS,
                         "GEP address descriptor is missing", &selection, gep);
        const auto addressNodes = state.gepAddressNodes.at(gep);
        if (!access->base || !access->addressRoot || !access->base->getType()->isPointerTy())
          return failure(LLVMFrontendStatus::UnsupportedPointerBase,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE,
                         "GEP has no loop-invariant pointer root", &selection, gep);
        if (llvm::isa<llvm::PHINode>(access->addressRoot)) {
          provider(*access->addressRoot, addressNodes.address, 0, false);
        } else if (state.nodes.contains(access->base)) {
          provider(*access->base, addressNodes.address, 0, false);
        } else {
          builder.bindExternal(addressNodes.address, 0,
                               getIfExternal(state, builder, *access->base, addressNodes.type));
        }
        if (access->dynamicTerms.empty()) {
          builder.bindConstant(addressNodes.address, 1,
                               getAddressConstant(state, builder, addressNodes.type,
                                                  access->gepConstantOffsetWords));
          continue;
        }

        for (std::size_t index = 0; index < access->dynamicTerms.size(); ++index) {
          provider(*access->dynamicTerms[index].value, addressNodes.termNodes[index], 0, false);
          builder.bindConstant(addressNodes.termNodes[index], 1,
                               getAddressConstant(state, builder, addressNodes.type,
                                                  access->dynamicTerms[index].scaleWords));
        }
        ir::NodeId expression = addressNodes.termNodes.front();
        std::size_t sumIndex = 0;
        for (std::size_t index = 1; index < addressNodes.termNodes.size(); ++index) {
          const auto sum = addressNodes.sumNodes[sumIndex++];
          builder.addDataEdge(expression, sum, 0);
          builder.addDataEdge(addressNodes.termNodes[index], sum, 1);
          expression = sum;
        }
        if (addressNodes.hasConstantSum) {
          const auto sum = addressNodes.sumNodes[sumIndex++];
          builder.addDataEdge(expression, sum, 0);
          builder.bindConstant(sum, 1,
                               getAddressConstant(state, builder, addressNodes.type,
                                                  access->gepConstantOffsetWords));
          expression = sum;
        }
        builder.addDataEdge(expression, addressNodes.address, 1);
        continue;
      }
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        provider(*load->getPointerOperand(), destination->second, 0, false);
        continue;
      }
      std::uint32_t operand = 0;
      for (const auto* value : semanticOperands(instruction)) {
        const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction);
        const bool swapConditionOperands = compare && compare == state.region.condition &&
                                           storePredicateComplemented &&
                                           complementSwapsOperands(*icmpPredicate(*compare));
        const auto sourceOperand = swapConditionOperands ? 1U - operand : operand;
        const llvm::Value* sourceValue =
            compare ? static_cast<const llvm::Value*>(compare->getOperand(sourceOperand)) : value;
        bool predicateOperand = false;
        if (llvm::isa<llvm::ICmpInst>(instruction))
          predicateOperand = false;
        else if (llvm::isa<llvm::SelectInst>(instruction))
          predicateOperand = operand == 0;
        else if (const auto operandType = valueType(*sourceValue);
                 operandType && operandType->kind == ir::ValueKind::Predicate)
          predicateOperand = true;
        const auto edge = provider(*sourceValue, destination->second, operand, predicateOperand);
        const bool constantPredicate = predicateOperand && llvm::isa<llvm::Constant>(sourceValue) &&
                                       constantBits(*sourceValue).has_value();
        if (predicateOperand && !edge && !constantPredicate)
          return failure(LLVMFrontendStatus::UnsupportedBranchCondition,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
                         "predicate operand " + valueSummary(*sourceValue) +
                             " has no Generic predicate provider",
                         &selection, &instruction);
        if (predicateOperand && edge) {
          const auto* directSelect = llvm::dyn_cast<llvm::SelectInst>(&instruction);
          if (directSelect) {
            const auto predicateNode = state.nodes.find(directSelect->getCondition());
            if (predicateNode != state.nodes.end()) {
              if (discoveredRegion && directSelect->getCondition() == state.region.condition &&
                  !state.provenance.ifConversions.empty()) {
                auto& plan = state.provenance.ifConversions.front();
                plan.predicateNode = predicateNode->second;
                plan.selects.push_back({valueSummary(*directSelect), destination->second,
                                        valueSummary(*directSelect->getTrueValue()),
                                        valueSummary(*directSelect->getFalseValue()), nullptr,
                                        directSelect, directSelect->getTrueValue(),
                                        directSelect->getFalseValue()});
                plan.predicateEdges.push_back(*edge);
                ++operand;
                continue;
              }
              LLVMIfConversionProvenance plan;
              plan.id = static_cast<std::uint32_t>(state.provenance.ifConversions.size());
              plan.conditionBlock = blockName(*selection.function, directSelect->getParent());
              plan.mergeBlock = plan.conditionBlock;
              plan.condition = valueSummary(*directSelect->getCondition());
              plan.predicateNode = predicateNode->second;
              plan.selects.push_back({valueSummary(*directSelect), destination->second,
                                      valueSummary(*directSelect->getTrueValue()),
                                      valueSummary(*directSelect->getFalseValue()), nullptr,
                                      directSelect, directSelect->getTrueValue(),
                                      directSelect->getFalseValue()});
              plan.predicateEdges.push_back(*edge);
              plan.conditionValue = directSelect->getCondition();
              state.provenance.ifConversions.push_back(std::move(plan));
            }
          }
        }
        ++operand;
      }
    }
  }

  for (const auto* store : stores) {
    const bool conditional = discoveredRegion && isConditionalStore(*store, state.region);
    const auto addressType = addressValueType(module, *store->getPointerOperand());
    const auto dataType = store->getValueOperand()->getType()->isPointerTy()
                              ? pointerTokenType(module, selection, *store->getValueOperand())
                              : valueType(*store->getValueOperand());
    if (!addressType || !dataType)
      return failure(LLVMFrontendStatus::DirectStoreAddressRequired,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DIRECT_STORE_ADDRESS_REQUIRED,
                     "predicated Store requires direct scalar address and data", &selection, store);
    if (!store->getPointerOperand()->getType()->isPointerTy())
      return failure(LLVMFrontendStatus::UnsupportedPointerBase,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE,
                     "Store address must be a supported LLVM pointer", &selection, store);
    std::vector<ir::ValueType> storeOperands{*addressType, *dataType};
    if (conditional)
      storeOperands.push_back(ir::ValueType::predicate());
    const auto storeNode = builder.addNode(
        ir::Opcode::Store, std::move(storeOperands), ir::ValueType::voidTy(), std::nullopt,
        ir::MemoryOpInfo{static_cast<std::uint32_t>(
                             store->getValueOperand()->getType()->getPrimitiveSizeInBits()),
                         false, static_cast<std::uint32_t>(store->getAlign().value())});
    state.nodes.emplace(store, storeNode);
    provider(*store->getPointerOperand(), storeNode, 0, false);
    const auto dataEdge = provider(*store->getValueOperand(), storeNode, 1, false);
    static_cast<void>(dataEdge);
    if (conditional) {
      auto predicate = state.nodes.find(state.region.condition);
      if (predicate == state.nodes.end())
        return failure(LLVMFrontendStatus::UnsupportedBranchCondition,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
                       "Store branch predicate was not lowered", &selection, store);
      const ir::NodeId predicateNode = predicate->second;
      const auto predicateEdge = builder.addPredicateEdge(predicateNode, storeNode, 2);
      for (auto& region : state.provenance.ifConversions) {
        if (!region.branch)
          continue;
        region.predicateNode = predicateNode;
        region.predicatedStores.push_back(storeNode);
        region.predicateEdges.push_back(predicateEdge);
      }
    }
  }

  for (const auto& access : memoryAnalysis.accesses) {
    const auto memoryNode = state.nodes.at(access.instruction);
    const auto addressNode = state.nodes.find(access.address);
    state.provenance.memoryAccesses.push_back(
        {access.id, std::string(toString(access.kind)), valueSummary(*access.base),
         std::string(toString(access.addressMode)), access.invariantExpression,
         access.constantOffsetBytes, access.iterationStrideBytes, access.constantOffsetWords,
         access.iterationStrideWords, access.accessWidthBits, access.alignmentBytes, memoryNode,
         addressNode == state.nodes.end() ? memoryNode : addressNode->second, access.instruction,
         access.base});
  }
  for (const auto& dependence : memoryAnalysis.dependences) {
    const auto source =
        state.nodes.at(memoryAnalysis.accesses.at(dependence.sourceAccess).instruction);
    const auto destination =
        state.nodes.at(memoryAnalysis.accesses.at(dependence.destinationAccess).instruction);
    const auto edge =
        builder.addMemoryEdge(source, destination, dependence.kind, dependence.distance);
    state.provenance.memoryDependences.push_back(
        {dependence.sourceAccess, dependence.destinationAccess,
         std::string(ir::toString(dependence.kind)), dependence.distance,
         std::string(toString(dependence.mode)), dependence.reason, edge});
  }

  auto addLiveOut = [&](const llvm::Value& value, ir::NodeId source) {
    bool badMerge = false;
    if (!hasOutsideUse(value, selection, badMerge) || badMerge)
      return;
    const auto type = valueType(value);
    if (!type || type->kind == ir::ValueKind::Predicate)
      return;
    const auto id = static_cast<ir::LiveOutId>(state.provenance.liveOuts.size());
    auto name = value.hasName() ? value.getName().str() : "liveout." + std::to_string(id);
    while (state.liveOutNames.contains(name))
      name += "." + std::to_string(id);
    state.liveOutNames.insert(name);
    builder.addLiveOut(name, *type, source);
    state.provenance.liveOuts.push_back({id, source, name, &value});
  };
  std::vector<std::pair<const llvm::Value*, ir::NodeId>> liveOutCandidates;
  for (const auto& [value, node] : state.nodes)
    liveOutCandidates.push_back({value, node});
  for (const auto& [value, node] : state.selects)
    liveOutCandidates.push_back({value, node});
  std::sort(liveOutCandidates.begin(), liveOutCandidates.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  for (const auto& [value, node] : liveOutCandidates)
    addLiveOut(*value, node);

  if (!state.provenance.liveOuts.empty() &&
      std::ranges::any_of(memoryAnalysis.accesses,
                          [](const auto& access) { return access.iterationStrideWords != 0; }))
    return failure(
        LLVMFrontendStatus::MemoryWithABIScalarLiveOutUnsupportedV0,
        LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MEMORY_WITH_ABI_SCALAR_LIVEOUT_UNSUPPORTED_V0,
        "dynamic user memory with an ABI scalar LiveOut is outside the V0 reserved-region proof",
        &selection);

  auto dfg = builder.finish();
  const auto verification = ir::DFGVerifier::verify(dfg);
  if (!verification.ok()) {
    std::string detail = verification.format();
    detail += "\nnode provenance:";
    std::vector<std::pair<ir::NodeId, std::string>> nodeProvenance;
    nodeProvenance.reserve(state.nodes.size());
    for (const auto& [value, node] : state.nodes)
      nodeProvenance.emplace_back(node, valueSummary(*value));
    std::sort(nodeProvenance.begin(), nodeProvenance.end());
    for (const auto& [node, value] : nodeProvenance)
      detail += " n" + std::to_string(node) + "=" + value;
    return failure(LLVMFrontendStatus::InvalidGenericDFG,
                   LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DFG_VERIFY_FAILED, std::move(detail),
                   &selection);
  }

  LLVMFrontendResult result;
  result.status = LLVMFrontendStatus::Success;
  result.dfg = std::move(dfg);
  LLVMFrontendMetadata metadata;
  metadata.functionName = selection.function->getName().str();
  metadata.loopHeader = blockName(*selection.function, selection.block);
  metadata.loopDepth = selection.loop->getLoopDepth();
  metadata.loopBlockCount = selection.loop->getBlocks().size();
  metadata.addressUnitBytes = options.addressUnitBytes;
  metadata.requiresTripCount = true;
  metadata.staticTripCount = inferStaticTripCount(selection);
  metadata.loopShape = selection.linearRegion ? "linear_multiblock" : "structured";
  metadata.loopEntryCanonicalized = selection.loopEntryCanonicalized;
  metadata.coalescedStorePairs = selection.coalescedStorePairs;
  metadata.forwardedBranchLoads = selection.forwardedBranchLoads;
  result.metadata = std::move(metadata);
  if (selection.linearRegion) {
    const auto& region = *selection.linearRegion;
    LLVMLinearLoopProvenance linear;
    linear.header = blockName(*selection.function, region.header);
    linear.preheader = blockName(*selection.function, region.preheader);
    linear.latch = blockName(*selection.function, region.latch);
    linear.exiting = blockName(*selection.function, region.exiting);
    linear.exit = blockName(*selection.function, region.exit);
    linear.terminationBlock = blockName(*selection.function, region.terminationBranch->getParent());
    for (const auto* block : region.orderedBlocks)
      linear.orderedBlocks.push_back(blockName(*selection.function, block));
    state.provenance.linearLoop = std::move(linear);
  }
  std::unordered_map<const llvm::Instruction*, std::uint32_t> ordinals;
  for (const auto& block : *selection.function) {
    std::uint32_t ordinal = 0;
    for (const auto& instruction : block)
      ordinals.emplace(&instruction, ordinal++);
  }
  for (const auto& [value, node] : state.nodes) {
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction)
      continue;
    const auto opcodeName =
        llvm::isa<llvm::GetElementPtrInst>(instruction) &&
                pointerGEPUpdates.contains(llvm::cast<llvm::GetElementPtrInst>(instruction))
            ? "POINTER_GEP_UPDATE"
            : instruction->getOpcodeName();
    state.provenance.nodes.push_back({node, selection.function->getName().str(),
                                      blockName(*selection.function, instruction->getParent()),
                                      ordinals.at(instruction), opcodeName, instruction});
  }
  for (const auto& synthetic : state.syntheticAddressNodes) {
    state.provenance.nodes.push_back({synthetic.node, selection.function->getName().str(),
                                      blockName(*selection.function, synthetic.gep->getParent()),
                                      ordinals.at(synthetic.gep), synthetic.role, synthetic.gep});
  }
  for (const auto* value : state.selectOrder) {
    const auto node = state.selects.at(value);
    state.provenance.nodes.push_back({node, selection.function->getName().str(),
                                      blockName(*selection.function, value->getParent()),
                                      ordinals.at(value), "PHI_SELECT", value});
  }
  for (const auto& [value, id] : state.externals) {
    const auto type = value->getType()->isPointerTy()
                          ? addressValueType(module, *value).value_or(ir::ValueType::i32())
                          : valueType(*value).value_or(ir::ValueType::i32());
    state.provenance.externals.push_back(
        {id, value->hasName() ? value->getName().str() : "", type.toString(), value});
  }
  std::sort(state.provenance.nodes.begin(), state.provenance.nodes.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.node < rhs.node; });
  std::sort(state.provenance.externals.begin(), state.provenance.externals.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.external < rhs.external; });
  result.provenance = std::move(state.provenance);
  return result;
}

LLVMFrontendResult lowerSelectedLoop(llvm::Module& module, const LLVMFrontendOptions& options) {
  LLVMFrontendResult error;
  auto selected = selectLoop(module, options, error);
  if (!selected)
    return error;
  if (!canonicalizeLoopEntry(*selected, error))
    return error;
  bool hasInternalBranch = false;
  bool hasDirectSelect = false;
  bool hasMemory = false;
  for (auto* block : selected->loop->getBlocks()) {
    if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator()))
      if (branch->isConditional() && selected->loop->contains(branch->getSuccessor(0)) &&
          selected->loop->contains(branch->getSuccessor(1)))
        hasInternalBranch = true;
    for (const auto& instruction : *block)
      if (llvm::isa<llvm::SelectInst>(instruction))
        hasDirectSelect = true;
      else if (isMemoryInstruction(instruction))
        hasMemory = true;
  }
  if (hasInternalBranch || hasDirectSelect || hasMemory || selected->loop->getBlocks().size() > 1) {
    auto region = discoverBranchRegion(*selected, error);
    if (hasInternalBranch && !region)
      return error;
    if (region) {
      forwardRedundantBranchLoads(*selected, *region);
      coalesceSameAddressStores(*selected, *region);
    }
    if (selected->loop->getBlocks().size() > 1 && !region) {
      const auto linear = discoverLinearLoopRegion(*selected->loop);
      if (!linear.ok()) {
        LLVMFrontendDiagnosticCode code =
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE;
        if (linear.status == LinearLoopStatus::NoPreheader)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER;
        else if (linear.status == LinearLoopStatus::NoLatch)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NO_LATCH;
        else if (linear.status == LinearLoopStatus::ExitShape)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_EXIT_SHAPE;
        else if (linear.status == LinearLoopStatus::InternalConditionalBranch)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_INTERNAL_BRANCH;
        else if (linear.status == LinearLoopStatus::UnsupportedTerminator)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_UNSUPPORTED_TERMINATOR;
        else if (linear.status == LinearLoopStatus::NonHeaderPHI)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NONHEADER_PHI;
        else if (linear.status == LinearLoopStatus::NonLinearCFG)
          code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NON_LINEAR_CFG;
        return failure(LLVMFrontendStatus::UnsupportedLoopShape, code, linear.message, &*selected);
      }
      selected->linearRegion = *linear.region;
    }
    return lowerStructuredLoop(module, options, *selected, std::move(region));
  }
  if (!shapeIsValid(*selected, error))
    return error;
  LoweringState state;
  state.selection = std::move(*selected);
  if (!buildControlSlice(state, error))
    return error;
  if (!discoverRecurrences(state, error))
    return error;
  promoteRecurrenceProducerClosure(state);
  if (!validateControlDataUses(state, error))
    return error;

  ir::DFGBuilder builder(state.selection.function->getName().str());

  std::uint32_t instructionOrdinal = 0;
  for (const auto& instruction : *state.selection.block) {
    if (ignoredInstruction(instruction) || instruction.isTerminator() ||
        state.controlSlice.contains(&instruction) || llvm::isa<llvm::PHINode>(instruction)) {
      ++instructionOrdinal;
      continue;
    }
    if (isMemoryInstruction(instruction)) {
      return failure(LLVMFrontendStatus::UnsupportedMemoryOperation,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY,
                     "memory operations are deferred to T018", &state.selection, &instruction);
    }
    const auto resultType = valueType(instruction);
    if (!resultType) {
      return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                     "LLVM data instruction must produce a supported integer type",
                     &state.selection, &instruction);
    }
    const auto mappedOpcode = opcode(instruction);
    const auto custom = customOperationKey(instruction);
    if (!mappedOpcode && !custom) {
      return failure(LLVMFrontendStatus::UnsupportedInstruction,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE,
                     "LLVM instruction is outside the T015 scalar subset", &state.selection,
                     &instruction);
    }
    std::vector<ir::ValueType> operandTypes;
    for (const auto* operand : semanticOperands(instruction)) {
      const auto operandType = valueType(*operand);
      if (!operandType) {
        return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                       "LLVM data operand must have a supported integer type", &state.selection,
                       &instruction);
      }
      operandTypes.push_back(*operandType);
    }
    const auto source = ir::SourceInfo{sourceLabel(
        *state.selection.function, *state.selection.block, instruction, instructionOrdinal)};
    const auto id =
        mappedOpcode ? builder.addNode(*mappedOpcode, std::move(operandTypes), *resultType,
                                       std::nullopt, std::nullopt, source)
                     : builder.addCustomNode(*custom, std::move(operandTypes), *resultType, source);
    state.nodes.emplace(&instruction, id);
    ++instructionOrdinal;
  }

  for (const auto& instruction : *state.selection.block) {
    if (!state.nodes.contains(&instruction))
      continue;
    const auto dst = state.nodes.at(&instruction);
    std::uint32_t operandIndex = 0;
    for (const auto* operand : semanticOperands(instruction)) {
      if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(operand)) {
        const auto recurrence = state.recurrences.find(phi);
        if (recurrence == state.recurrences.end())
          return failure(LLVMFrontendStatus::UnsupportedLoopCarriedPHI,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LOOP_CARRIED_PHI,
                         "data instruction consumes an unsupported loop-carried PHI " +
                             valueSummary(*phi),
                         &state.selection, &instruction);
        auto& descriptor = state.provenance.recurrences[recurrence->second];
        const auto sourceIterator = state.nodes.find(descriptor.backedge);
        if (sourceIterator == state.nodes.end())
          return failure(LLVMFrontendStatus::UnsupportedRecurrenceProvider,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_PRODUCER,
                         "recurrence backedge provider was not lowered to a Generic node",
                         &state.selection, &instruction);
        ir::RecurrenceBoundary boundary;
        const auto initialType = valueType(*descriptor.initial);
        if (!initialType)
          return failure(LLVMFrontendStatus::UnsupportedRecurrenceType,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE,
                         "recurrence boundary has an unsupported type", &state.selection,
                         &instruction);
        ir::ExternalOperandBinding initialBinding;
        if (const auto* constant = llvm::dyn_cast<llvm::Constant>(descriptor.initial);
            constant && constantBits(*constant)) {
          initialBinding = ir::ConstantRef{getConstant(state, builder, *constant, *initialType)};
        } else {
          initialBinding =
              ir::ExternalValueRef{getExternal(state, builder, *descriptor.initial, *initialType)};
        }
        boundary.values.push_back({0, initialBinding});
        const auto edge =
            builder.addDataEdge(sourceIterator->second, dst, operandIndex, 1, std::move(boundary));
        descriptor.uses.push_back({valueSummary(instruction), operandIndex, dst, edge});
      } else if (const auto* producer = llvm::dyn_cast<llvm::Instruction>(operand)) {
        const auto iterator = state.nodes.find(producer);
        if (iterator != state.nodes.end()) {
          builder.addDataEdge(iterator->second, dst, operandIndex);
        } else if (state.selection.loop->contains(producer)) {
          return failure(LLVMFrontendStatus::UnsupportedInductionDataUse,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INDUCTION_DATA_USE,
                         "data instruction consumes an unsupported in-loop value", &state.selection,
                         &instruction);
        } else {
          const auto type = valueType(*operand);
          if (!type)
            return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                           LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                           "external operand has an unsupported integer type", &state.selection,
                           &instruction);
          const auto external = getExternal(state, builder, *operand, *type);
          builder.bindExternal(dst, operandIndex, external);
        }
      } else if (const auto* constant = llvm::dyn_cast<llvm::Constant>(operand);
                 constant && constantBits(*constant)) {
        const auto type = valueType(*operand);
        if (!type)
          return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                         "constant operand has an unsupported type", &state.selection,
                         &instruction);
        builder.bindConstant(dst, operandIndex, getConstant(state, builder, *constant, *type));
      } else if (llvm::isa<llvm::Constant>(operand)) {
        return failure(LLVMFrontendStatus::UnsupportedInstruction,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE,
                       "constant operand kind is unsupported", &state.selection, &instruction);
      } else {
        const auto type = valueType(*operand);
        if (!type)
          return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                         LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                         "external operand has an unsupported type", &state.selection,
                         &instruction);
        const auto external = getExternal(state, builder, *operand, *type);
        builder.bindExternal(dst, operandIndex, external);
      }
      ++operandIndex;
    }
  }

  auto dfg = builder.finish();
  for (const auto& instruction : *state.selection.block) {
    const auto iterator = state.nodes.find(&instruction);
    if (iterator == state.nodes.end())
      continue;
    bool badMerge = false;
    if (hasOutsideUse(instruction, state.selection, badMerge)) {
      auto name = instruction.hasName()
                      ? instruction.getName().str()
                      : "liveout." + std::to_string(state.provenance.liveOuts.size());
      if (state.liveOutNames.contains(name))
        name += "." + std::to_string(state.provenance.liveOuts.size());
      state.liveOutNames.insert(name);
      const auto type = valueType(instruction);
      if (!type)
        return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                       "live-out must have a supported integer type", &state.selection,
                       &instruction);
      const auto liveOut = dfg.liveOuts().size();
      ir::DFGBuilder rebuilt(dfg.name());
      for (const auto& external : dfg.externalValues())
        rebuilt.importExternal(external);
      for (const auto& constant : dfg.constants())
        rebuilt.importConstant(constant);
      for (const auto& node : dfg.nodes())
        rebuilt.importNode(node);
      for (const auto& binding : dfg.externalBindings())
        std::visit(
            [&](const auto& source) {
              using Source = std::decay_t<decltype(source)>;
              if constexpr (std::is_same_v<Source, ir::ExternalValueRef>)
                rebuilt.bindExternal(binding.node, binding.operand, source.value);
              else
                rebuilt.bindConstant(binding.node, binding.operand, source.value);
            },
            binding.source);
      for (const auto& edge : dfg.edges())
        rebuilt.importEdge(edge);
      for (const auto& existing : dfg.liveOuts())
        rebuilt.importLiveOut(existing);
      rebuilt.addLiveOut(name, *type, iterator->second);
      dfg = rebuilt.finish();
      state.provenance.liveOuts.push_back(
          {static_cast<ir::LiveOutId>(liveOut), iterator->second, name, &instruction});
    } else if (badMerge) {
      return failure(LLVMFrontendStatus::UnsupportedExitMerge,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_EXIT_MERGE,
                     "exit PHI merges more than one loop value", &state.selection, &instruction);
    }
  }

  const auto report = ir::DFGVerifier::verify(dfg);
  if (!report.ok())
    return failure(LLVMFrontendStatus::InvalidGenericDFG,
                   LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DFG_VERIFY_FAILED, report.format(),
                   &state.selection);

  LLVMFrontendResult result;
  result.status = LLVMFrontendStatus::Success;
  result.dfg = std::move(dfg);
  LLVMFrontendMetadata metadata;
  metadata.functionName = state.selection.function->getName().str();
  metadata.loopHeader = blockName(*state.selection.function, *state.selection.block);
  metadata.loopDepth = state.selection.loop->getLoopDepth();
  metadata.loopBlockCount = state.selection.loop->getBlocks().size();
  metadata.addressUnitBytes = options.addressUnitBytes;
  metadata.requiresTripCount = true;
  metadata.staticTripCount = inferStaticTripCount(state.selection);
  result.metadata = std::move(metadata);

  instructionOrdinal = 0;
  for (const auto& instruction : *state.selection.block) {
    const auto iterator = state.nodes.find(&instruction);
    if (iterator == state.nodes.end()) {
      ++instructionOrdinal;
      continue;
    }
    result.provenance.nodes.push_back({iterator->second, state.selection.function->getName().str(),
                                       blockName(*state.selection.function, *state.selection.block),
                                       instructionOrdinal, instruction.getOpcodeName(),
                                       &instruction});
    ++instructionOrdinal;
  }
  std::vector<std::pair<const llvm::Value*, ir::ExternalValueId>> orderedExternals(
      state.externals.begin(), state.externals.end());
  std::sort(orderedExternals.begin(), orderedExternals.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  for (const auto& [value, id] : orderedExternals)
    result.provenance.externals.push_back(
        {id, value->hasName() ? value->getName().str() : "",
         valueType(*value) ? valueType(*value)->toString() : "unsupported", value});
  result.provenance.liveOuts = std::move(state.provenance.liveOuts);
  result.provenance.recurrences = std::move(state.provenance.recurrences);
  std::sort(state.provenance.controlSlice.begin(), state.provenance.controlSlice.end());
  result.provenance.controlSlice = std::move(state.provenance.controlSlice);
  return result;
}

} // namespace

std::string_view toString(LLVMFrontendStatus status) noexcept {
  switch (status) {
  case LLVMFrontendStatus::Success:
    return "success";
  case LLVMFrontendStatus::ParseFailure:
    return "parse_failure";
  case LLVMFrontendStatus::FunctionNotFound:
    return "function_not_found";
  case LLVMFrontendStatus::NoInnermostLoop:
    return "no_innermost_loop";
  case LLVMFrontendStatus::AmbiguousLoopSelection:
    return "ambiguous_loop_selection";
  case LLVMFrontendStatus::UnsupportedLoopShape:
    return "unsupported_loop_shape";
  case LLVMFrontendStatus::UnsupportedLLVMType:
    return "unsupported_llvm_type";
  case LLVMFrontendStatus::UnsupportedInstruction:
    return "unsupported_instruction";
  case LLVMFrontendStatus::UnsupportedMemoryOperation:
    return "unsupported_memory_operation";
  case LLVMFrontendStatus::UnsupportedControlFlow:
    return "unsupported_control_flow";
  case LLVMFrontendStatus::UnsupportedLoopCarriedPHI:
    return "unsupported_loop_carried_phi";
  case LLVMFrontendStatus::UnsupportedInductionDataUse:
    return "unsupported_induction_data_use";
  case LLVMFrontendStatus::UnsupportedRecurrenceShape:
    return "unsupported_recurrence_shape";
  case LLVMFrontendStatus::UnsupportedRecurrenceType:
    return "unsupported_recurrence_type";
  case LLVMFrontendStatus::UnsupportedRecurrenceProvider:
    return "unsupported_recurrence_provider";
  case LLVMFrontendStatus::UnsupportedPhiToPhiUse:
    return "unsupported_phi_to_phi_use";
  case LLVMFrontendStatus::UnsupportedPhiLiveOutSemantics:
    return "unsupported_phi_liveout_semantics";
  case LLVMFrontendStatus::UnsupportedBranchRegion:
    return "unsupported_branch_region";
  case LLVMFrontendStatus::MultipleInternalBranches:
    return "multiple_internal_branches";
  case LLVMFrontendStatus::NestedPredicationUnsupported:
    return "nested_predication_unsupported";
  case LLVMFrontendStatus::BranchNoUniqueMerge:
    return "branch_no_unique_merge";
  case LLVMFrontendStatus::UnsupportedBranchCondition:
    return "unsupported_branch_condition";
  case LLVMFrontendStatus::UnsupportedPredicateComplement:
    return "unsupported_predicate_complement";
  case LLVMFrontendStatus::UnsafeSpeculation:
    return "unsafe_speculation";
  case LLVMFrontendStatus::UnsupportedControlMerge:
    return "unsupported_control_merge";
  case LLVMFrontendStatus::UnsupportedPredicateMerge:
    return "unsupported_predicate_merge";
  case LLVMFrontendStatus::UnsupportedIfSideEffect:
    return "unsupported_if_side_effect";
  case LLVMFrontendStatus::PredicatedLoadUnsupported:
    return "predicated_load_unsupported";
  case LLVMFrontendStatus::MemoryPatternRequiresT018:
    return "memory_pattern_requires_t018";
  case LLVMFrontendStatus::UnsupportedMemoryType:
    return "unsupported_memory_type";
  case LLVMFrontendStatus::UnsupportedMemoryAlignment:
    return "unsupported_memory_alignment";
  case LLVMFrontendStatus::UnsupportedMemoryAddressSpace:
    return "unsupported_memory_address_space";
  case LLVMFrontendStatus::UnsupportedPointerBase:
    return "unsupported_pointer_base";
  case LLVMFrontendStatus::UnsupportedNonAffineAddress:
    return "unsupported_non_affine_address";
  case LLVMFrontendStatus::UnsupportedPathSensitiveMemoryOrder:
    return "unsupported_path_sensitive_memory_order";
  case LLVMFrontendStatus::MemoryWithABIScalarLiveOutUnsupportedV0:
    return "memory_with_abi_scalar_liveout_unsupported_v0";
  case LLVMFrontendStatus::DirectStoreAddressRequired:
    return "direct_store_address_required";
  case LLVMFrontendStatus::MultipleStoresRequireT018:
    return "multiple_stores_require_t018";
  case LLVMFrontendStatus::ConditionalRecurrenceUnsupported:
    return "conditional_recurrence_unsupported";
  case LLVMFrontendStatus::UnsupportedExitMerge:
    return "unsupported_exit_merge";
  case LLVMFrontendStatus::DataDependentLoopControl:
    return "data_dependent_loop_control";
  case LLVMFrontendStatus::InvalidGenericDFG:
    return "invalid_generic_dfg";
  case LLVMFrontendStatus::VerificationFailure:
    return "verification_failure";
  case LLVMFrontendStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string_view toString(LLVMFrontendDiagnosticCode code) noexcept {
  switch (code) {
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_FUNCTION_NOT_FOUND:
    return "LLVM_FRONTEND_FUNCTION_NOT_FOUND";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_NO_INNERMOST_LOOP:
    return "LLVM_FRONTEND_NO_INNERMOST_LOOP";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_AMBIGUOUS_LOOP:
    return "LLVM_FRONTEND_AMBIGUOUS_LOOP";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE:
    return "LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE:
    return "LLVM_FRONTEND_UNSUPPORTED_TYPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE:
    return "LLVM_FRONTEND_UNSUPPORTED_OPCODE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY:
    return "LLVM_FRONTEND_UNSUPPORTED_MEMORY";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_CONTROL_FLOW:
    return "LLVM_FRONTEND_UNSUPPORTED_CONTROL_FLOW";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LOOP_CARRIED_PHI:
    return "LLVM_FRONTEND_LOOP_CARRIED_PHI";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INDUCTION_DATA_USE:
    return "LLVM_FRONTEND_INDUCTION_DATA_USE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE:
    return "LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE:
    return "LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE:
    return "LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_PRODUCER:
    return "LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_PRODUCER";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PHI_TO_PHI_USE:
    return "LLVM_FRONTEND_PHI_TO_PHI_USE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PHI_LIVEOUT_SEMANTICS:
    return "LLVM_FRONTEND_PHI_LIVEOUT_SEMANTICS";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_REGION:
    return "LLVM_FRONTEND_UNSUPPORTED_BRANCH_REGION";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MULTIPLE_INTERNAL_BRANCHES:
    return "LLVM_FRONTEND_MULTIPLE_INTERNAL_BRANCHES";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_NESTED_PREDICATION_UNSUPPORTED:
    return "LLVM_FRONTEND_NESTED_PREDICATION_UNSUPPORTED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_BRANCH_NO_UNIQUE_MERGE:
    return "LLVM_FRONTEND_BRANCH_NO_UNIQUE_MERGE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION:
    return "LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PREDICATE_COMPLEMENT:
    return "LLVM_FRONTEND_UNSUPPORTED_PREDICATE_COMPLEMENT";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSAFE_SPECULATION:
    return "LLVM_FRONTEND_UNSAFE_SPECULATION";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_CONTROL_MERGE:
    return "LLVM_FRONTEND_UNSUPPORTED_CONTROL_MERGE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PREDICATE_MERGE:
    return "LLVM_FRONTEND_UNSUPPORTED_PREDICATE_MERGE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT:
    return "LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_PREDICATED_LOAD_UNSUPPORTED:
    return "LLVM_FRONTEND_PREDICATED_LOAD_UNSUPPORTED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MEMORY_PATTERN_REQUIRES_T018:
    return "LLVM_FRONTEND_MEMORY_PATTERN_REQUIRES_T018";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE:
    return "LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_ALIGNMENT:
    return "LLVM_FRONTEND_UNSUPPORTED_MEMORY_ALIGNMENT";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_MEMORY_ADDRESS_SPACE:
    return "LLVM_FRONTEND_UNSUPPORTED_MEMORY_ADDRESS_SPACE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE:
    return "LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS:
    return "LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_PATH_SENSITIVE_MEMORY_ORDER:
    return "LLVM_FRONTEND_UNSUPPORTED_PATH_SENSITIVE_MEMORY_ORDER";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MEMORY_WITH_ABI_SCALAR_LIVEOUT_UNSUPPORTED_V0:
    return "LLVM_FRONTEND_MEMORY_WITH_ABI_SCALAR_LIVEOUT_UNSUPPORTED_V0";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DIRECT_STORE_ADDRESS_REQUIRED:
    return "LLVM_FRONTEND_DIRECT_STORE_ADDRESS_REQUIRED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_MULTIPLE_STORES_REQUIRE_T018:
    return "LLVM_FRONTEND_MULTIPLE_STORES_REQUIRE_T018";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_CONDITIONAL_RECURRENCE_UNSUPPORTED:
    return "LLVM_FRONTEND_CONDITIONAL_RECURRENCE_UNSUPPORTED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_IFCONV_VERIFY_FAILED:
    return "LLVM_FRONTEND_IFCONV_VERIFY_FAILED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED:
    return "LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_RECURRENCE_BOUNDARY_VERIFY_FAILED:
    return "LLVM_FRONTEND_RECURRENCE_BOUNDARY_VERIFY_FAILED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_EXIT_MERGE:
    return "LLVM_FRONTEND_EXIT_MERGE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DATA_DEPENDENT_CONTROL:
    return "LLVM_FRONTEND_DATA_DEPENDENT_CONTROL";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_DFG_VERIFY_FAILED:
    return "LLVM_FRONTEND_DFG_VERIFY_FAILED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_VERIFY_FAILED:
    return "LLVM_FRONTEND_VERIFY_FAILED";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER:
    return "LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NO_LATCH:
    return "LLVM_FRONTEND_LINEAR_LOOP_NO_LATCH";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_EXIT_SHAPE:
    return "LLVM_FRONTEND_LINEAR_LOOP_EXIT_SHAPE";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_INTERNAL_BRANCH:
    return "LLVM_FRONTEND_LINEAR_LOOP_INTERNAL_BRANCH";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_UNSUPPORTED_TERMINATOR:
    return "LLVM_FRONTEND_LINEAR_LOOP_UNSUPPORTED_TERMINATOR";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NON_LINEAR_CFG:
    return "LLVM_FRONTEND_LINEAR_LOOP_NON_LINEAR_CFG";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_LINEAR_LOOP_NONHEADER_PHI:
    return "LLVM_FRONTEND_LINEAR_LOOP_NONHEADER_PHI";
  case LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INTERNAL_ERROR:
    return "LLVM_FRONTEND_INTERNAL_ERROR";
  }
  return "LLVM_FRONTEND_INTERNAL_ERROR";
}

std::string LLVMFrontendResult::toJson() const {
  Json root{{"schema", "cgra.llvm_frontend.result.v1"},
            {"status", toString(status)},
            {"message", message},
            {"diagnostics", Json::array()}};
  if (metadata) {
    root["metadata"] = {{"function", metadata->functionName},
                        {"loop_header", metadata->loopHeader},
                        {"loop_depth", metadata->loopDepth},
                        {"loop_block_count", metadata->loopBlockCount},
                        {"address_unit_bytes", metadata->addressUnitBytes},
                        {"loop_shape", metadata->loopShape},
                        {"loop_entry_canonicalized", metadata->loopEntryCanonicalized},
                        {"coalesced_store_pairs", metadata->coalescedStorePairs},
                        {"forwarded_branch_loads", metadata->forwardedBranchLoads},
                        {"inlined_pure_helper_calls", metadata->inlinedPureHelperCalls},
                        {"requires_trip_count", metadata->requiresTripCount}};
    if (metadata->staticTripCount)
      root["metadata"]["static_trip_count"] = *metadata->staticTripCount;
    else
      root["metadata"]["static_trip_count"] = nullptr;
  }
  if (dfg)
    root["dfg"] = Json::parse(ir::toJson(*dfg));
  root["provenance"] = Json{{"control_slice", provenance.controlSlice},
                            {"nodes", Json::array()},
                            {"externals", Json::array()},
                            {"live_outs", Json::array()},
                            {"recurrences", Json::array()},
                            {"if_conversions", Json::array()},
                            {"memory_accesses", Json::array()},
                            {"memory_dependences", Json::array()}};
  if (provenance.linearLoop) {
    const auto& linear = *provenance.linearLoop;
    root["provenance"]["linear_loop"] = {{"schema", "cgra.llvm_linear_loop.v1"},
                                         {"header", linear.header},
                                         {"preheader", linear.preheader},
                                         {"latch", linear.latch},
                                         {"exiting", linear.exiting},
                                         {"exit", linear.exit},
                                         {"termination_block", linear.terminationBlock},
                                         {"ordered_blocks", linear.orderedBlocks}};
  }
  for (const auto& node : provenance.nodes)
    root["provenance"]["nodes"].push_back({{"node", node.node},
                                           {"function", node.function},
                                           {"basic_block", node.basicBlock},
                                           {"instruction_ordinal", node.instructionOrdinal},
                                           {"opcode", node.opcode}});
  for (const auto& external : provenance.externals)
    root["provenance"]["externals"].push_back({{"external", external.external},
                                               {"value", external.valueName},
                                               {"type", external.valueType}});
  for (const auto& liveOut : provenance.liveOuts)
    root["provenance"]["live_outs"].push_back({{"live_out", liveOut.liveOut},
                                               {"source_node", liveOut.sourceNode},
                                               {"value", liveOut.valueName}});
  for (const auto& recurrence : provenance.recurrences) {
    Json item = {{"id", recurrence.id},
                 {"phi", recurrence.phi},
                 {"type", recurrence.type},
                 {"preheader", recurrence.preheader},
                 {"initial_value", recurrence.initialValue},
                 {"latch", recurrence.latch},
                 {"backedge_value", recurrence.backedgeValue},
                 {"distance", recurrence.distance},
                 {"uses", Json::array()}};
    for (const auto& use : recurrence.uses)
      item["uses"].push_back({{"consumer", use.consumer},
                              {"operand", use.operand},
                              {"destination", use.destination},
                              {"edge", use.edge}});
    root["provenance"]["recurrences"].push_back(std::move(item));
  }
  for (const auto& region : provenance.ifConversions) {
    Json item = {{"id", region.id},
                 {"condition_block", region.conditionBlock},
                 {"true_block", region.trueBlock},
                 {"false_block", region.falseBlock},
                 {"merge_block", region.mergeBlock},
                 {"condition", region.condition},
                 {"predicate_complemented", region.predicateComplemented},
                 {"predicate_node", region.predicateNode},
                 {"selects", Json::array()},
                 {"predicated_stores", region.predicatedStores},
                 {"predicate_edges", region.predicateEdges}};
    for (const auto& select : region.selects)
      item["selects"].push_back({{"phi", select.phi},
                                 {"node", select.node},
                                 {"true_value", select.trueValue},
                                 {"false_value", select.falseValue}});
    root["provenance"]["if_conversions"].push_back(std::move(item));
  }
  for (const auto& access : provenance.memoryAccesses)
    root["provenance"]["memory_accesses"].push_back(
        {{"id", access.id},
         {"kind", access.kind},
         {"base", access.base},
         {"address_mode", access.addressMode},
         {"invariant_expression", access.invariantExpression},
         {"offset_bytes", access.offsetBytes},
         {"stride_bytes", access.strideBytes},
         {"offset_words", access.offsetWords},
         {"stride_words", access.strideWords},
         {"access_width_bits", access.accessWidthBits},
         {"alignment_bytes", access.alignmentBytes},
         {"memory_node", access.memoryNode},
         {"address_provider", access.addressProvider}});
  for (const auto& dependence : provenance.memoryDependences)
    root["provenance"]["memory_dependences"].push_back(
        {{"source_access", dependence.sourceAccess},
         {"destination_access", dependence.destinationAccess},
         {"kind", dependence.kind},
         {"distance", dependence.distance},
         {"mode", dependence.mode},
         {"reason", dependence.reason},
         {"edge", dependence.edge}});
  for (const auto& diagnostic : diagnostics)
    root["diagnostics"].push_back({{"code", toString(diagnostic.code)},
                                   {"message", diagnostic.message},
                                   {"function", diagnostic.function},
                                   {"loop_header", diagnostic.loopHeader},
                                   {"instruction", diagnostic.instruction}});
  return root.dump(2) + '\n';
}

LLVMFrontendResult lowerInnermostLoop(const llvm::Module& module,
                                      const LLVMFrontendOptions& options) {
  try {
    auto normalized = std::shared_ptr<llvm::Module>(llvm::CloneModule(module).release());
    materializeConstantAddressExpressions(*normalized);
    const auto inlinedPureHelperCalls = inlineSmallPureHelpers(*normalized);
    auto result = lowerSelectedLoop(*normalized, options);
    if (result.ok()) {
      result.metadata->inlinedPureHelperCalls = inlinedPureHelperCalls;
      result.normalizedModule = std::move(normalized);
    }
    return result;
  } catch (const std::exception& error) {
    return failure(LLVMFrontendStatus::InternalError,
                   LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INTERNAL_ERROR, error.what());
  }
}

} // namespace cgra::frontend::llvm_frontend
