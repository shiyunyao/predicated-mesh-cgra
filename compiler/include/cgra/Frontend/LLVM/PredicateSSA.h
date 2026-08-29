// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class BasicBlock;
class Loop;
class Value;
} // namespace llvm

namespace cgra::frontend::llvm_frontend {

struct PredicateExpression {
  enum class Kind { True, False, Value, Not, And, Or };

  Kind kind = Kind::True;
  const llvm::Value* value = nullptr;
  std::vector<std::shared_ptr<const PredicateExpression>> operands;
};

enum class PredicateSSAStatus {
  Success,
  MissingHeader,
  NonBooleanCondition,
  UnsupportedTerminator,
  DynamicExit,
  NonReducibleBody,
};

std::string_view toString(PredicateSSAStatus status) noexcept;
std::string predicateExpressionKey(const PredicateExpression& expression);

struct BlockPredicate {
  const llvm::BasicBlock* block = nullptr;
  std::shared_ptr<const PredicateExpression> expression;
};

struct PredicateSSAResult {
  PredicateSSAStatus status = PredicateSSAStatus::NonReducibleBody;
  std::string message;
  std::vector<BlockPredicate> blockPredicates;

  bool ok() const noexcept { return status == PredicateSSAStatus::Success; }
  const BlockPredicate* forBlock(const llvm::BasicBlock* block) const noexcept;
};

// Builds predicates for the loop-body CFG without changing LLVM IR.  Loop
// backedges and the unique normal loop-exit edge are control boundaries; only
// branches whose two successors remain in the selected loop contribute a
// predicate expression.  The body must be a reducible DAG after backedge
// removal, which makes the result deterministic and suitable for lowering to
// Generic AND/OR/NOT/Select operations.
PredicateSSAResult buildPredicateSSA(const llvm::Loop& loop);

} // namespace cgra::frontend::llvm_frontend
