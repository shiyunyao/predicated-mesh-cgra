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

#include <algorithm>
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
      const auto id = std::get<ir::ConstantRef>(binding.source).value;
      if (result.dfg->containsConstant(id) &&
          result.dfg->constant(id).bits == constant->getValue().getZExtValue() &&
          valueType(*constant) && result.dfg->constant(id).type == *valueType(*constant))
        return true;
    }
  }
  return false;
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
      if (!sawWaw)
        report.add("LLVM_FRONTEND_IFCONV_VERIFY_FAILED",
                   "predicated Store is missing its distance-one self-WAW");
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
  const auto selection = select(module, options, report);
  if (!selection)
    return report;
  if (!result.provenance.ifConversions.empty())
    return verifyIfConvertedResult(module, options, result);
  if (!selection->branch || selection->loop->getBlocks().size() != 1) {
    report.add("LLVM_FRONTEND_LOOP_SHAPE_MISMATCH", "selected loop shape changed after lowering");
    return report;
  }
  auto slice = controlSlice(*selection);
  for (const auto& recurrence : result.provenance.recurrences)
    if (const auto* backedge = llvm::dyn_cast_or_null<llvm::Instruction>(recurrence.backedge))
      slice.erase(backedge);
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
