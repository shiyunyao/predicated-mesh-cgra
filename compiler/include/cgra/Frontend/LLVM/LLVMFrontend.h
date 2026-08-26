// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Frontend/LLVM/LLVMMemoryAnalysis.h"
#include "cgra/IR/DFG.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class Instruction;
class BranchInst;
class Module;
class PHINode;
class SelectInst;
class StoreInst;
class Value;
} // namespace llvm

namespace cgra::frontend::llvm_frontend {

enum class LLVMFrontendStatus {
  Success,
  ParseFailure,
  FunctionNotFound,
  NoInnermostLoop,
  AmbiguousLoopSelection,
  UnsupportedLoopShape,
  UnsupportedLLVMType,
  UnsupportedInstruction,
  UnsupportedMemoryOperation,
  UnsupportedControlFlow,
  UnsupportedLoopCarriedPHI,
  UnsupportedInductionDataUse,
  UnsupportedRecurrenceShape,
  UnsupportedRecurrenceType,
  UnsupportedRecurrenceProvider,
  UnsupportedPhiToPhiUse,
  UnsupportedPhiLiveOutSemantics,
  UnsupportedBranchRegion,
  MultipleInternalBranches,
  NestedPredicationUnsupported,
  BranchNoUniqueMerge,
  UnsupportedBranchCondition,
  UnsupportedPredicateComplement,
  UnsafeSpeculation,
  UnsupportedControlMerge,
  UnsupportedPredicateMerge,
  UnsupportedIfSideEffect,
  PredicatedLoadUnsupported,
  MemoryPatternRequiresT018,
  UnsupportedMemoryType,
  UnsupportedMemoryAlignment,
  UnsupportedMemoryAddressSpace,
  UnsupportedPointerBase,
  UnsupportedNonAffineAddress,
  UnsupportedPathSensitiveMemoryOrder,
  MemoryWithABIScalarLiveOutUnsupportedV0,
  DirectStoreAddressRequired,
  MultipleStoresRequireT018,
  ConditionalRecurrenceUnsupported,
  UnsupportedExitMerge,
  DataDependentLoopControl,
  InvalidGenericDFG,
  VerificationFailure,
  InternalError,
};

std::string_view toString(LLVMFrontendStatus status) noexcept;

enum class LLVMFrontendDiagnosticCode {
  LLVM_FRONTEND_FUNCTION_NOT_FOUND,
  LLVM_FRONTEND_NO_INNERMOST_LOOP,
  LLVM_FRONTEND_AMBIGUOUS_LOOP,
  LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE,
  LLVM_FRONTEND_UNSUPPORTED_TYPE,
  LLVM_FRONTEND_UNSUPPORTED_OPCODE,
  LLVM_FRONTEND_UNSUPPORTED_MEMORY,
  LLVM_FRONTEND_UNSUPPORTED_CONTROL_FLOW,
  LLVM_FRONTEND_LOOP_CARRIED_PHI,
  LLVM_FRONTEND_INDUCTION_DATA_USE,
  LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_SHAPE,
  LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_TYPE,
  LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_INITIAL_VALUE,
  LLVM_FRONTEND_UNSUPPORTED_RECURRENCE_PRODUCER,
  LLVM_FRONTEND_PHI_TO_PHI_USE,
  LLVM_FRONTEND_PHI_LIVEOUT_SEMANTICS,
  LLVM_FRONTEND_UNSUPPORTED_BRANCH_REGION,
  LLVM_FRONTEND_MULTIPLE_INTERNAL_BRANCHES,
  LLVM_FRONTEND_NESTED_PREDICATION_UNSUPPORTED,
  LLVM_FRONTEND_BRANCH_NO_UNIQUE_MERGE,
  LLVM_FRONTEND_UNSUPPORTED_BRANCH_CONDITION,
  LLVM_FRONTEND_UNSUPPORTED_PREDICATE_COMPLEMENT,
  LLVM_FRONTEND_UNSAFE_SPECULATION,
  LLVM_FRONTEND_UNSUPPORTED_CONTROL_MERGE,
  LLVM_FRONTEND_UNSUPPORTED_PREDICATE_MERGE,
  LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT,
  LLVM_FRONTEND_PREDICATED_LOAD_UNSUPPORTED,
  LLVM_FRONTEND_MEMORY_PATTERN_REQUIRES_T018,
  LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE,
  LLVM_FRONTEND_UNSUPPORTED_MEMORY_ALIGNMENT,
  LLVM_FRONTEND_UNSUPPORTED_MEMORY_ADDRESS_SPACE,
  LLVM_FRONTEND_UNSUPPORTED_POINTER_BASE,
  LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS,
  LLVM_FRONTEND_UNSUPPORTED_PATH_SENSITIVE_MEMORY_ORDER,
  LLVM_FRONTEND_MEMORY_WITH_ABI_SCALAR_LIVEOUT_UNSUPPORTED_V0,
  LLVM_FRONTEND_DIRECT_STORE_ADDRESS_REQUIRED,
  LLVM_FRONTEND_MULTIPLE_STORES_REQUIRE_T018,
  LLVM_FRONTEND_CONDITIONAL_RECURRENCE_UNSUPPORTED,
  LLVM_FRONTEND_IFCONV_VERIFY_FAILED,
  LLVM_FRONTEND_RECURRENCE_EDGE_VERIFY_FAILED,
  LLVM_FRONTEND_RECURRENCE_BOUNDARY_VERIFY_FAILED,
  LLVM_FRONTEND_EXIT_MERGE,
  LLVM_FRONTEND_DATA_DEPENDENT_CONTROL,
  LLVM_FRONTEND_DFG_VERIFY_FAILED,
  LLVM_FRONTEND_VERIFY_FAILED,
  LLVM_FRONTEND_INTERNAL_ERROR,
};

std::string_view toString(LLVMFrontendDiagnosticCode code) noexcept;

struct LLVMFrontendDiagnostic {
  LLVMFrontendDiagnosticCode code = LLVMFrontendDiagnosticCode::LLVM_FRONTEND_INTERNAL_ERROR;
  std::string message;
  std::string function;
  std::string loopHeader;
  std::string instruction;
};

struct LLVMFrontendOptions {
  std::string functionName;
  std::optional<std::string> loopHeader;
};

struct LLVMFrontendMetadata {
  std::string functionName;
  std::string loopHeader;
  std::uint32_t loopDepth = 0;
  std::uint32_t loopBlockCount = 0;
  bool requiresTripCount = true;
  std::optional<std::uint64_t> staticTripCount;
};

struct LLVMFrontendNodeProvenance {
  ir::NodeId node = 0;
  std::string function;
  std::string basicBlock;
  std::uint32_t instructionOrdinal = 0;
  std::string opcode;
  const llvm::Instruction* instruction = nullptr;
};

struct LLVMFrontendExternalProvenance {
  ir::ExternalValueId external = 0;
  std::string valueName;
  std::string valueType;
  const llvm::Value* value = nullptr;
};

struct LLVMFrontendLiveOutProvenance {
  ir::LiveOutId liveOut = 0;
  ir::NodeId sourceNode = 0;
  std::string valueName;
  const llvm::Value* value = nullptr;
};

struct LLVMRecurrenceUseProvenance {
  std::string consumer;
  std::uint32_t operand = 0;
  ir::NodeId destination = 0;
  ir::EdgeId edge = 0;
};

struct LLVMRecurrenceProvenance {
  std::uint32_t id = 0;
  std::string phi;
  std::string type;
  std::string preheader;
  std::string initialValue;
  std::string latch;
  std::string backedgeValue;
  std::uint32_t distance = 1;
  std::vector<LLVMRecurrenceUseProvenance> uses;
  const llvm::PHINode* phiValue = nullptr;
  const llvm::Value* initial = nullptr;
  const llvm::Value* backedge = nullptr;
};

struct LLVMIfConversionSelectProvenance {
  std::string phi;
  ir::NodeId node = 0;
  std::string trueValue;
  std::string falseValue;
  const llvm::PHINode* phiValue = nullptr;
  const llvm::SelectInst* selectValue = nullptr;
  const llvm::Value* trueProvider = nullptr;
  const llvm::Value* falseProvider = nullptr;
};

struct LLVMIfConversionProvenance {
  std::uint32_t id = 0;
  std::string conditionBlock;
  std::string trueBlock;
  std::string falseBlock;
  std::string mergeBlock;
  std::string condition;
  bool predicateComplemented = false;
  ir::NodeId predicateNode = 0;
  std::vector<LLVMIfConversionSelectProvenance> selects;
  std::vector<ir::NodeId> predicatedStores;
  std::vector<ir::EdgeId> predicateEdges;
  const llvm::BranchInst* branch = nullptr;
  const llvm::Value* conditionValue = nullptr;
};

struct LLVMMemoryAccessProvenance {
  std::uint32_t id = 0;
  std::string kind;
  std::string base;
  std::int64_t offsetWords = 0;
  std::int64_t strideWords = 0;
  std::uint32_t accessWidthBits = 0;
  ir::NodeId memoryNode = 0;
  ir::NodeId addressProvider = 0;
  const llvm::Instruction* instruction = nullptr;
  const llvm::Value* baseValue = nullptr;
};

struct LLVMMemoryDependenceProvenance {
  std::uint32_t sourceAccess = 0;
  std::uint32_t destinationAccess = 0;
  std::string kind;
  std::uint32_t distance = 0;
  std::string mode;
  std::string reason;
  ir::EdgeId edge = 0;
};

struct LLVMFrontendProvenance {
  std::vector<LLVMFrontendNodeProvenance> nodes;
  std::vector<LLVMFrontendExternalProvenance> externals;
  std::vector<LLVMFrontendLiveOutProvenance> liveOuts;
  std::vector<LLVMRecurrenceProvenance> recurrences;
  std::vector<LLVMIfConversionProvenance> ifConversions;
  std::vector<LLVMMemoryAccessProvenance> memoryAccesses;
  std::vector<LLVMMemoryDependenceProvenance> memoryDependences;
  std::vector<std::string> controlSlice;
};

struct LLVMFrontendResult {
  LLVMFrontendStatus status = LLVMFrontendStatus::InternalError;
  std::string message;
  std::optional<ir::DFG> dfg;
  std::optional<LLVMFrontendMetadata> metadata;
  LLVMFrontendProvenance provenance;
  std::vector<LLVMFrontendDiagnostic> diagnostics;

  bool ok() const noexcept { return status == LLVMFrontendStatus::Success && dfg.has_value(); }
  std::string toJson() const;
};

LLVMFrontendResult lowerInnermostLoop(const llvm::Module& module,
                                      const LLVMFrontendOptions& options);

} // namespace cgra::frontend::llvm_frontend
