// SPDX-License-Identifier: MIT
#include "cgra/ABI/CompileKernel.h"
#include "cgra/ABI/KernelABIVerifier.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
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

std::uint32_t nextConstantId(const ir::DFG& source) {
  std::uint32_t next = 0;
  for (const auto& constant : source.constants()) {
    if (constant.id == std::numeric_limits<std::uint32_t>::max())
      throw std::overflow_error("constant ID space exhausted");
    next = std::max(next, constant.id + 1);
  }
  return next;
}

KernelInvocationValidationResult validateResearchInvocation(
    const KernelSignature& signature, const KernelInvocation& invocation,
    const TargetModel& target) {
  if (invocation.tripCount == 0)
    return {false, "ABI_INVALID_TRIP_COUNT: trip count must be positive"};
  std::set<ir::ExternalValueId> seen;
  for (const auto& input : invocation.scalarInputs) {
    if (!seen.insert(input.id).second)
      return {false, "ABI_DUPLICATE_INPUT: duplicate scalar input"};
    const auto descriptor = std::ranges::find(signature.scalarInputs, input.id,
                                              &KernelInputDesc::id);
    if (descriptor == signature.scalarInputs.end())
      return {false, "ABI_UNKNOWN_INPUT: scalar input is not in the kernel signature"};
    if (!target.supportsValueType(descriptor->type))
      return {false, "ABI_UNSUPPORTED_INPUT_TYPE: mapping target rejects input type"};
    if (descriptor->type.bitWidth < 64 && (input.bits >> descriptor->type.bitWidth) != 0)
      return {false, "ABI_INPUT_TYPE_MISMATCH: scalar bits exceed the declared type"};
  }
  if (seen.size() != signature.scalarInputs.size())
    return {false, "ABI_MISSING_INPUT: invocation does not bind every scalar input"};
  for (const auto& [address, value] : invocation.scratchpadPreload) {
    static_cast<void>(value);
    if (address >= target.memory().depth)
      return {false, "ABI_SCRATCHPAD_ADDRESS_OUT_OF_RANGE: preload exceeds mapping memory"};
  }
  return {true, {}};
}

ABIBoundKernel specializeForMappingResearch(const ir::DFG& source,
                                            const KernelSignature& signature,
                                            const KernelInvocation& invocation,
                                            const TargetModel& target) {
  ir::DFGBuilder builder(source.name());
  for (const auto& value : source.externalValues())
    builder.importExternal(value);
  for (const auto& value : source.constants())
    builder.importConstant(value);
  for (const auto& node : source.nodes())
    builder.importNode(node);
  for (const auto& liveOut : source.liveOuts())
    builder.importLiveOut(liveOut);

  std::map<ir::ExternalValueId, ir::ConstantId> specialized;
  const auto firstConstant = nextConstantId(source);
  for (const auto& descriptor : signature.scalarInputs) {
    const auto input = std::ranges::find(invocation.scalarInputs, descriptor.id,
                                         &KernelScalarInputValue::id);
    if (input == invocation.scalarInputs.end())
      throw std::invalid_argument("validated research invocation lost a scalar input");
    const auto constant = firstConstant + static_cast<ir::ConstantId>(specialized.size());
    builder.importConstant({constant, descriptor.type, input->bits});
    specialized.emplace(descriptor.id, constant);
  }

  for (const auto& sourceEdge : source.edges()) {
    auto edge = sourceEdge;
    auto rewriteBoundary = [&](auto& info) {
      if (!info.boundary)
        return;
      for (auto& item : info.boundary->values) {
        const auto* external = std::get_if<ir::ExternalValueRef>(&item.value);
        if (external)
          item.value = ir::ConstantRef{specialized.at(external->value)};
      }
    };
    if (edge.kind() == ir::Edge::Kind::Data)
      rewriteBoundary(std::get<ir::DataEdgeInfo>(edge.info));
    else if (edge.kind() == ir::Edge::Kind::Predicate)
      rewriteBoundary(std::get<ir::PredicateEdgeInfo>(edge.info));
    builder.importEdge(std::move(edge));
  }
  for (const auto& binding : source.externalBindings()) {
    if (const auto* external = std::get_if<ir::ExternalValueRef>(&binding.source))
      builder.bindConstant(binding.node, binding.operand, specialized.at(external->value));
    else
      builder.bindConstant(binding.node, binding.operand,
                           std::get<ir::ConstantRef>(binding.source).value);
  }

  KernelABILayout layout{target.memory().depth, target.memory().depth,
                         target.memory().depth, {}, {}};
  for (const auto& [external, constant] : specialized)
    layout.inputs.push_back({external, constant});
  auto dfg = builder.finish();
  const auto report = ir::DFGVerifier::verify(dfg);
  if (!report.ok())
    throw std::runtime_error("mapping specialization produced invalid DFG: " + report.format());
  return {std::move(dfg), signature, invocation, std::move(layout)};
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
  if (options.backend.mode == pipeline::CompileDFGMode::MappingResearch) {
    const auto invocationReport =
        validateResearchInvocation(*result.signature, options.invocation, target);
    if (!invocationReport.ok()) {
      result.status = KernelCompileStatus::InvalidInvocation;
      result.message = invocationReport.message;
      return result;
    }
    try {
      result.bound = specializeForMappingResearch(source, *result.signature,
                                                  options.invocation, target);
      result.abiLayout = result.bound->layout;
    } catch (const std::exception& error) {
      result.status = KernelCompileStatus::ABIBindingFailure;
      result.message = error.what();
      return result;
    }
  } else {
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
    if (options.backend.mode == pipeline::CompileDFGMode::MappingResearch)
      writeArtifact(verificationPath,
                    "{\"schema\":\"cgra.mapping_specialization.verification.v1\","
                    "\"valid\":true,\"hardware_abi_outputs_materialized\":false}\n");
    else
      writeArtifact(verificationPath,
                    KernelABIVerifier::verify(source, target, options.invocation, *result.bound)
                        .toJson());
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
  result.message = options.backend.mode == pipeline::CompileDFGMode::MappingResearch
                       ? "mapping specialization and modulo mapping succeeded"
                       : "kernel ABI binding and backend compilation succeeded";
  result.artifacts.insert(result.artifacts.end(), backend.artifacts.begin(),
                          backend.artifacts.end());
  if (!artifactDirectory.empty())
    writeArtifact(artifactDirectory / "kernel_compile_result.json", result.toJson());
  return result;
}
} // namespace cgra::abi
