// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/FrontendInvocationValidation.h"

#include <nlohmann/json.hpp>

namespace cgra::frontend::llvm_frontend {

FrontendInvocationValidationReport
validateFrontendInvocation(const LLVMFrontendMetadata& frontend,
                           const abi::KernelInvocation& invocation) {
  if (frontend.staticTripCount && *frontend.staticTripCount != invocation.tripCount) {
    return {FrontendInvocationValidationStatus::FrontendInvocationMismatch,
            "FrontendInvocationMismatch: static trip count " +
                std::to_string(*frontend.staticTripCount) +
                " does not match invocation trip count " + std::to_string(invocation.tripCount)};
  }
  return {FrontendInvocationValidationStatus::Valid, {}};
}

std::string FrontendInvocationValidationReport::toJson() const {
  return nlohmann::json{{"schema", "cgra.llvm_frontend.invocation_validation.v1"},
                        {"status", ok() ? "valid" : "frontend_invocation_mismatch"},
                        {"message", message}}
             .dump(2) +
         "\n";
}

} // namespace cgra::frontend::llvm_frontend
