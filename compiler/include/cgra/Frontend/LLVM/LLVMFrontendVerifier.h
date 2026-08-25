// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Frontend/LLVM/LLVMFrontend.h"

#include <string>
#include <vector>

namespace cgra::frontend::llvm_frontend {

struct LLVMFrontendVerificationDiagnostic {
  std::string code;
  std::string message;
};

class LLVMFrontendVerificationReport {
public:
  bool ok() const noexcept { return diagnostics_.empty(); }
  void add(std::string code, std::string message);
  const std::vector<LLVMFrontendVerificationDiagnostic>& diagnostics() const noexcept {
    return diagnostics_;
  }
  std::string format() const;
  std::string toJson() const;

private:
  std::vector<LLVMFrontendVerificationDiagnostic> diagnostics_;
};

LLVMFrontendVerificationReport verifyFrontendResult(const llvm::Module& module,
                                                    const LLVMFrontendOptions& options,
                                                    const LLVMFrontendResult& result);

} // namespace cgra::frontend::llvm_frontend
