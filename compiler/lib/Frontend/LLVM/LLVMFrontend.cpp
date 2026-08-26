// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

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
  std::map<std::pair<std::uint16_t, std::uint64_t>, ir::ConstantId> constants;
  std::set<std::string> externalNames;
  std::set<std::string> liveOutNames;
  std::uint32_t nextExternalOrdinal = 0;
  LLVMFrontendProvenance provenance;
};

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
  const auto* integer = llvm::dyn_cast<llvm::IntegerType>(value.getType());
  if (!integer || integer->getBitWidth() == 0 || integer->getBitWidth() > 64)
    return std::nullopt;
  if (integer->getBitWidth() == 1)
    return ir::ValueType::predicate();
  return ir::ValueType::integer(static_cast<std::uint16_t>(integer->getBitWidth()));
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

std::string valueSummary(const llvm::Value& value) {
  if (value.hasName())
    return "%" + value.getName().str();
  if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(&value)) {
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
      if (!opcode(*userInstruction))
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
    if (!llvm::isa<llvm::ConstantInt>(initial)) {
      if (llvm::isa<llvm::Constant>(initial))
        return recurrenceFailure(
            error, LLVMFrontendStatus::UnsupportedRecurrenceProvider,
            LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE,
            "recurrence initial provider must be ConstantInt or loop-external scalar",
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
        llvm::isa<llvm::PHINode>(backedgeInstruction) || !opcode(*backedgeInstruction))
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
        !opcode(*instruction)) {
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
  while (!work.empty()) {
    const auto* instruction = work.back();
    work.pop_back();
    if (!state.controlSlice.erase(instruction))
      continue;
    for (const auto& operand : instruction->operands()) {
      const auto* dependency = llvm::dyn_cast<llvm::Instruction>(operand.get());
      if (!dependency || !state.selection.loop->contains(dependency) ||
          llvm::isa<llvm::PHINode>(dependency) || !opcode(*dependency))
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
                           const llvm::ConstantInt& constant, const ir::ValueType& type) {
  const auto key = std::make_pair(type.bitWidth, constant.getValue().getZExtValue());
  if (const auto iterator = state.constants.find(key); iterator != state.constants.end())
    return iterator->second;
  const auto id = builder.addConstant(type, key.second);
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

LLVMFrontendResult lowerSelectedLoop(llvm::Module& module, const LLVMFrontendOptions& options) {
  LLVMFrontendResult error;
  auto selected = selectLoop(module, options, error);
  if (!selected)
    return error;
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
    if (!mappedOpcode) {
      return failure(LLVMFrontendStatus::UnsupportedInstruction,
                     LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_OPCODE,
                     "LLVM instruction is outside the T015 scalar subset", &state.selection,
                     &instruction);
    }
    std::vector<ir::ValueType> operandTypes;
    for (const auto& operand : instruction.operands()) {
      const auto operandType = valueType(*operand);
      if (!operandType) {
        return failure(LLVMFrontendStatus::UnsupportedLLVMType,
                       LLVMFrontendDiagnosticCode::LLVM_FRONTEND_UNSUPPORTED_TYPE,
                       "LLVM data operand must have a supported integer type", &state.selection,
                       &instruction);
      }
      operandTypes.push_back(*operandType);
    }
    const auto id = builder.addNode(
        *mappedOpcode, std::move(operandTypes), *resultType, std::nullopt, std::nullopt,
        ir::SourceInfo{sourceLabel(*state.selection.function, *state.selection.block, instruction,
                                   instructionOrdinal)});
    state.nodes.emplace(&instruction, id);
    ++instructionOrdinal;
  }

  for (const auto& instruction : *state.selection.block) {
    if (!state.nodes.contains(&instruction))
      continue;
    const auto dst = state.nodes.at(&instruction);
    std::uint32_t operandIndex = 0;
    for (const auto& operand : instruction.operands()) {
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
        if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(descriptor.initial)) {
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
      } else if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
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
                       "only ConstantInt operands are supported in T015", &state.selection,
                       &instruction);
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
  metadata.requiresTripCount = true;
  llvm::Function& function = *state.selection.function;
  auto& dominatorTree = *state.selection.dominatorTree;
  auto& loopInfo = *state.selection.loopInfo;
  llvm::TargetLibraryInfoImpl libraryInfoImpl;
  llvm::TargetLibraryInfo libraryInfo(libraryInfoImpl);
  llvm::AssumptionCache assumptions(function);
  llvm::ScalarEvolution scalarEvolution(function, libraryInfo, assumptions, dominatorTree,
                                        loopInfo);
  if (const auto tripCount = scalarEvolution.getSmallConstantTripCount(state.selection.loop);
      tripCount != 0)
    metadata.staticTripCount = tripCount;
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
                            {"recurrences", Json::array()}};
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
    return lowerSelectedLoop(const_cast<llvm::Module&>(module), options);
  } catch (const std::exception& error) {
    return failure(LLVMFrontendStatus::InternalError,
                   LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INTERNAL_ERROR, error.what());
  }
}

} // namespace cgra::frontend::llvm_frontend
