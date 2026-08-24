// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelABIBinder.h"
#include "cgra/Pipeline/CompileDFG.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::abi {

struct KernelABIOptions {
  std::string kernelName;
};

struct CompileKernelOptions {
  KernelInvocation invocation;
  KernelABIOptions abi;
  pipeline::CompileDFGOptions backend;
};

enum class KernelCompileStatus {
  Success,
  InvalidSourceDFG,
  InvalidInvocation,
  ABIBindingFailure,
  ABIVerificationFailure,
  BackendCompilationFailure,
  InternalError,
};

std::string_view toString(KernelCompileStatus status) noexcept;

struct CompileKernelResult {
  KernelCompileStatus status = KernelCompileStatus::InternalError;
  std::string message;
  std::optional<KernelSignature> signature;
  std::optional<KernelABILayout> abiLayout;
  std::optional<ABIBoundKernel> bound;
  std::optional<pipeline::CompileDFGResult> backend;
  std::vector<std::filesystem::path> artifacts;
  bool ok() const noexcept {
    return status == KernelCompileStatus::Success && backend && backend->ok();
  }
  std::string toJson() const;
};

CompileKernelResult compileKernel(const ir::DFG& source, const TargetModel& target,
                                  const CompileKernelOptions& options);

} // namespace cgra::abi
