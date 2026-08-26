// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelInvocation.h"
#include "cgra/Frontend/LLVM/LLVMFrontend.h"

#include <string>

namespace cgra::frontend::llvm_frontend {

enum class FrontendInvocationValidationStatus {
  Valid,
  FrontendInvocationMismatch,
};

struct FrontendInvocationValidationReport {
  FrontendInvocationValidationStatus status =
      FrontendInvocationValidationStatus::FrontendInvocationMismatch;
  std::string message;

  bool ok() const noexcept { return status == FrontendInvocationValidationStatus::Valid; }
  std::string toJson() const;
};

FrontendInvocationValidationReport
validateFrontendInvocation(const LLVMFrontendMetadata& frontend,
                           const abi::KernelInvocation& invocation);

} // namespace cgra::frontend::llvm_frontend
