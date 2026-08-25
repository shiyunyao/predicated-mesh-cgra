// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"

#include "cgra/IR/DFGVerifier.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <sstream>
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
  if (!selection.branch || !selection.branch->isConditional())
    return result;
  std::vector<const llvm::Value*> work{selection.branch->getCondition()};
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
  const auto selection = select(module, options, report);
  if (!selection)
    return report;
  if (!selection->branch || selection->loop->getBlocks().size() != 1) {
    report.add("LLVM_FRONTEND_LOOP_SHAPE_MISMATCH", "selected loop shape changed after lowering");
    return report;
  }
  const auto slice = controlSlice(*selection);
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
    if (edge.kind() != ir::Edge::Kind::Data || edge.distance != 0) {
      report.add("LLVM_FRONTEND_EDGE_SEMANTICS_MISMATCH",
                 "T015 edges must be distance-zero Data edges");
      continue;
    }
    const auto* source = nodeProvenance(result, edge.src);
    const auto* destination = nodeProvenance(result, edge.dst);
    if (!source || !destination || !source->instruction || !destination->instruction) {
      report.add("LLVM_FRONTEND_EDGE_PROVENANCE_MISSING", "edge endpoint provenance is missing");
      continue;
    }
    const auto operand = std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
    if (operand >= destination->instruction->getNumOperands() ||
        destination->instruction->getOperand(operand) != source->instruction) {
      report.add("LLVM_FRONTEND_EDGE_PROVENANCE_INVALID",
                 "Generic data edge does not match the LLVM SSA def-use operand");
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
    if (instruction.isTerminator() || ignored(instruction) || slice.contains(&instruction))
      continue;
    if (!mappedInstructions.contains(&instruction))
      report.add("LLVM_FRONTEND_SILENT_INSTRUCTION_LOSS",
                 "semantic LLVM instruction is absent from Generic DFG provenance");
  }
  return report;
}

} // namespace cgra::frontend::llvm_frontend
