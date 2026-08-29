// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/PredicateSSA.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace cgra::frontend::llvm_frontend {
namespace {

using Expression = std::shared_ptr<const PredicateExpression>;

Expression make(PredicateExpression::Kind kind, const llvm::Value* value = nullptr,
                std::vector<Expression> operands = {}) {
  auto expression = std::make_shared<PredicateExpression>();
  expression->kind = kind;
  expression->value = value;
  expression->operands = std::move(operands);
  return expression;
}

Expression conjunction(Expression lhs, Expression rhs) {
  if (!lhs || lhs->kind == PredicateExpression::Kind::True)
    return rhs;
  if (!rhs || rhs->kind == PredicateExpression::Kind::True)
    return lhs;
  if (lhs->kind == PredicateExpression::Kind::False ||
      rhs->kind == PredicateExpression::Kind::False)
    return make(PredicateExpression::Kind::False);
  return make(PredicateExpression::Kind::And, nullptr, {std::move(lhs), std::move(rhs)});
}

Expression disjunction(std::vector<Expression> operands) {
  std::vector<Expression> filtered;
  for (auto& operand : operands) {
    if (!operand || operand->kind == PredicateExpression::Kind::False)
      continue;
    if (operand->kind == PredicateExpression::Kind::True)
      return make(PredicateExpression::Kind::True);
    filtered.push_back(std::move(operand));
  }
  if (filtered.empty())
    return make(PredicateExpression::Kind::False);
  if (filtered.size() == 1)
    return filtered.front();
  return make(PredicateExpression::Kind::Or, nullptr, std::move(filtered));
}

Expression edgePredicate(Expression parent, const llvm::BasicBlock& source,
                          const llvm::BasicBlock& destination, const llvm::Loop& loop,
                          PredicateSSAResult& result) {
  const auto* branch = llvm::dyn_cast<llvm::BranchInst>(source.getTerminator());
  if (!branch) {
    result.status = PredicateSSAStatus::UnsupportedTerminator;
    result.message = "loop body terminator is not a branch";
    return nullptr;
  }
  if (!branch->isConditional())
    return parent;
  const auto* condition = branch->getCondition();
  if (!condition->getType()->isIntegerTy(1)) {
    result.status = PredicateSSAStatus::NonBooleanCondition;
    result.message = "predicate branch condition is not i1";
    return nullptr;
  }
  const bool bothInLoop = loop.contains(branch->getSuccessor(0)) &&
                          loop.contains(branch->getSuccessor(1));
  // A branch with one outside successor is the loop termination boundary. It
  // controls whether another iteration starts, not whether this iteration's
  // body block executes, so it must not predicate its in-loop successor.
  if (!bothInLoop)
    return parent;
  if (branch->getSuccessor(0) == &destination)
    return conjunction(std::move(parent), make(PredicateExpression::Kind::Value, condition));
  if (branch->getSuccessor(1) == &destination)
    return conjunction(std::move(parent),
                       make(PredicateExpression::Kind::Not, nullptr,
                            {make(PredicateExpression::Kind::Value, condition)}));
  result.status = PredicateSSAStatus::NonReducibleBody;
  result.message = "branch successor is not the selected in-loop destination";
  return nullptr;
}

} // namespace

std::string_view toString(PredicateSSAStatus status) noexcept {
  switch (status) {
  case PredicateSSAStatus::Success:
    return "success";
  case PredicateSSAStatus::MissingHeader:
    return "missing_header";
  case PredicateSSAStatus::NonBooleanCondition:
    return "non_boolean_condition";
  case PredicateSSAStatus::UnsupportedTerminator:
    return "unsupported_terminator";
  case PredicateSSAStatus::DynamicExit:
    return "dynamic_exit";
  case PredicateSSAStatus::NonReducibleBody:
    return "non_reducible_body";
  }
  return "non_reducible_body";
}

std::string predicateExpressionKey(const PredicateExpression& expression) {
  switch (expression.kind) {
  case PredicateExpression::Kind::True:
    return "true";
  case PredicateExpression::Kind::False:
    return "false";
  case PredicateExpression::Kind::Value:
    if (expression.value && expression.value->hasName())
      return expression.value->getName().str();
    return "value";
  case PredicateExpression::Kind::Not:
    return "not(" + predicateExpressionKey(*expression.operands.front()) + ")";
  case PredicateExpression::Kind::And:
    return "and(" + predicateExpressionKey(*expression.operands[0]) + "," +
           predicateExpressionKey(*expression.operands[1]) + ")";
  case PredicateExpression::Kind::Or: {
    std::string result = "or(";
    for (std::size_t index = 0; index < expression.operands.size(); ++index) {
      if (index != 0)
        result += ",";
      result += predicateExpressionKey(*expression.operands[index]);
    }
    return result + ")";
  }
  }
  return "unknown";
}

const BlockPredicate* PredicateSSAResult::forBlock(const llvm::BasicBlock* block) const noexcept {
  const auto found = std::ranges::find(blockPredicates, block, &BlockPredicate::block);
  return found == blockPredicates.end() ? nullptr : &*found;
}

PredicateSSAResult buildPredicateSSA(const llvm::Loop& loop) {
  PredicateSSAResult result;
  const auto* header = loop.getHeader();
  if (!header) {
    result.status = PredicateSSAStatus::MissingHeader;
    result.message = "selected loop has no header";
    return result;
  }

  std::unordered_map<const llvm::BasicBlock*, std::size_t> layout;
  std::size_t ordinal = 0;
  for (const auto& block : *header->getParent())
    if (loop.contains(&block))
      layout.emplace(&block, ordinal++);

  std::unordered_map<const llvm::BasicBlock*, std::size_t> indegree;
  for (const auto* block : loop.blocks())
    indegree.emplace(block, 0);
  for (const auto* block : loop.blocks()) {
    const auto* terminator = block->getTerminator();
    if (!terminator || (!llvm::isa<llvm::BranchInst>(terminator))) {
      result.status = PredicateSSAStatus::UnsupportedTerminator;
      result.message = "predicate SSA requires branch-only loop body CFG";
      return result;
    }
    for (const auto* successor : llvm::successors(block)) {
      if (!loop.contains(successor) || successor == header)
        continue;
      ++indegree[successor];
    }
  }
  std::set<std::pair<std::size_t, const llvm::BasicBlock*>> ready;
  for (const auto& [block, degree] : indegree)
    if (degree == 0)
      ready.emplace(layout.at(block), block);
  std::vector<const llvm::BasicBlock*> order;
  while (!ready.empty()) {
    const auto [_, block] = *ready.begin();
    ready.erase(ready.begin());
    order.push_back(block);
    for (const auto* successor : llvm::successors(block)) {
      if (!loop.contains(successor) || successor == header)
        continue;
      auto& degree = indegree.at(successor);
      if (--degree == 0)
        ready.emplace(layout.at(successor), successor);
    }
  }
  if (order.size() != indegree.size()) {
    result.status = PredicateSSAStatus::NonReducibleBody;
    result.message = "loop body is cyclic after removing the header backedge";
    return result;
  }

  std::unordered_map<const llvm::BasicBlock*, Expression> predicates;
  predicates.emplace(header, make(PredicateExpression::Kind::True));
  for (const auto* block : order) {
    if (block == header)
      continue;
    std::vector<Expression> incoming;
    for (const auto* predecessor : llvm::predecessors(block)) {
      if (!loop.contains(predecessor) || predecessor == loop.getLoopLatch())
        continue;
      const auto found = predicates.find(predecessor);
      if (found == predicates.end()) {
        result.status = PredicateSSAStatus::NonReducibleBody;
        result.message = "loop body has a side entry or missing predecessor predicate";
        return result;
      }
      auto edge = edgePredicate(found->second, *predecessor, *block, loop, result);
      if (!edge)
        return result;
      incoming.push_back(std::move(edge));
    }
    if (incoming.empty()) {
      result.status = PredicateSSAStatus::NonReducibleBody;
      result.message = "loop body block has no forward predicate predecessor";
      return result;
    }
    predicates.emplace(block, disjunction(std::move(incoming)));
  }

  result.status = PredicateSSAStatus::Success;
  result.message.clear();
  for (const auto* block : order)
    result.blockPredicates.push_back({block, predicates.at(block)});
  return result;
}

} // namespace cgra::frontend::llvm_frontend
