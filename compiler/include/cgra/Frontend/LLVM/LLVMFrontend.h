// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class Instruction;
class Module;
class PHINode;
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

struct LLVMFrontendProvenance {
  std::vector<LLVMFrontendNodeProvenance> nodes;
  std::vector<LLVMFrontendExternalProvenance> externals;
  std::vector<LLVMFrontendLiveOutProvenance> liveOuts;
  std::vector<LLVMRecurrenceProvenance> recurrences;
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
