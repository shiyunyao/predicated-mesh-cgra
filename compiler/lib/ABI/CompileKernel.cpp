// SPDX-License-Identifier: MIT
#include "cgra/ABI/CompileKernel.h"
#include "cgra/ABI/KernelABIVerifier.h"

#include "cgra/IR/DFGSerialization.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace cgra::abi {

namespace {
void writeArtifact(const std::filesystem::path& path, std::string_view contents) {
  if (path.empty())
    return;
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("cannot write kernel ABI artifact " + temporary);
    output << contents;
    if (contents.empty() || contents.back() != '\n')
      output << '\n';
    if (!output)
      throw std::runtime_error("cannot flush kernel ABI artifact " + temporary);
  }
  std::filesystem::rename(temporary, path);
}
} // namespace

std::string_view toString(KernelCompileStatus status) noexcept {
  switch (status) {
  case KernelCompileStatus::Success:
    return "success";
  case KernelCompileStatus::InvalidSourceDFG:
    return "invalid_source_dfg";
  case KernelCompileStatus::InvalidInvocation:
    return "invalid_invocation";
  case KernelCompileStatus::ABIBindingFailure:
    return "abi_binding_failure";
  case KernelCompileStatus::ABIVerificationFailure:
    return "abi_verification_failure";
  case KernelCompileStatus::BackendCompilationFailure:
    return "backend_compilation_failure";
  case KernelCompileStatus::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

std::string CompileKernelResult::toJson() const {
  nlohmann::json root = {{"schema", "cgra.kernel_compile.result.v1"},
                         {"status", toString(status)},
                         {"message", message},
                         {"artifacts", nlohmann::json::array()}};
  if (signature)
    root["signature"] = nlohmann::json::parse(cgra::abi::toJson(*signature));
  if (abiLayout)
    root["abi_layout"] = nlohmann::json::parse(cgra::abi::toJson(*abiLayout));
  if (backend)
    root["backend"] = nlohmann::json::parse(backend->toJson());
  for (const auto& artifact : artifacts)
    root["artifacts"].push_back(artifact.string());
  return root.dump(2) + "\n";
}

CompileKernelResult compileKernel(const ir::DFG& source, const TargetModel& target,
                                  const CompileKernelOptions& options) {
  CompileKernelResult result;
  result.signature = inferSignature(source);
  if (!options.abi.kernelName.empty())
    result.signature->kernelName = options.abi.kernelName;
  const auto bound =
      KernelABIBinder::bind(source, target, options.invocation, result.signature->kernelName);
  if (!bound.ok()) {
    result.status = bound.status == KernelABIBindingStatus::InvalidSourceDFG
                        ? KernelCompileStatus::InvalidSourceDFG
                    : bound.status == KernelABIBindingStatus::InvalidInvocation
                        ? KernelCompileStatus::InvalidInvocation
                        : KernelCompileStatus::ABIBindingFailure;
    result.message = bound.format();
    return result;
  }
  result.bound = bound.bound;
  result.abiLayout = bound.bound->layout;
  const auto verification =
      KernelABIVerifier::verify(source, target, options.invocation, *result.bound);
  if (!verification.ok()) {
    result.status = KernelCompileStatus::ABIVerificationFailure;
    result.message = verification.format();
    return result;
  }

  std::filesystem::path artifactDirectory = options.backend.artifactDirectory;
  if (!artifactDirectory.empty()) {
    std::filesystem::create_directories(artifactDirectory);
    const auto sourcePath = artifactDirectory / "00_source.generic_dfg.json";
    const auto signaturePath = artifactDirectory / "01_kernel_signature.json";
    const auto invocationPath = artifactDirectory / "02_kernel_invocation.json";
    const auto boundPath = artifactDirectory / "03_abi_bound.generic_dfg.json";
    const auto layoutPath = artifactDirectory / "04_kernel_abi_layout.json";
    const auto verificationPath = artifactDirectory / "05_abi_verification.json";
    writeArtifact(sourcePath, ir::toJson(source));
    writeArtifact(signaturePath, toJson(*result.signature));
    writeArtifact(invocationPath, toJson(options.invocation, &*result.signature));
    writeArtifact(boundPath, ir::toJson(result.bound->dfg));
    writeArtifact(layoutPath, toJson(*result.abiLayout, &*result.signature, &options.invocation));
    writeArtifact(verificationPath, verification.toJson());
    result.artifacts = {sourcePath, signaturePath, invocationPath,
                        boundPath,  layoutPath,    verificationPath};
  }

  auto backendOptions = options.backend;
  backendOptions.tripCount = options.invocation.tripCount;
  backendOptions.scratchpadPreload = options.invocation.scratchpadPreload;
  if (backendOptions.programName.empty())
    backendOptions.programName = result.signature->kernelName;
  if (!backendOptions.artifactDirectory.empty())
    backendOptions.artifactDirectory /= "backend";
  const auto backend = pipeline::compileGenericDFG(result.bound->dfg, target, backendOptions);
  result.backend = backend;
  if (!backend.ok()) {
    result.status = KernelCompileStatus::BackendCompilationFailure;
    result.message = backend.message;
    if (!artifactDirectory.empty())
      writeArtifact(artifactDirectory / "kernel_compile_result.json", result.toJson());
    return result;
  }
  result.status = KernelCompileStatus::Success;
  result.message = "kernel ABI binding and backend compilation succeeded";
  result.artifacts.insert(result.artifacts.end(), backend.artifacts.begin(),
                          backend.artifacts.end());
  if (!artifactDirectory.empty())
    writeArtifact(artifactDirectory / "kernel_compile_result.json", result.toJson());
  return result;
}
} // namespace cgra::abi
