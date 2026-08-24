// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelABIBinder.h"

#include "cgra/ABI/KernelABIVerifier.h"
#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/DFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace cgra::abi {
namespace {
void add(KernelABIBindingResult& result, std::string code, std::string message) {
  result.diagnostics.push_back(
      {std::move(code), std::move(message), std::nullopt, std::nullopt, std::nullopt});
}
std::uint32_t nextId(std::span<const ir::ConstantValue> values) {
  std::uint32_t next = 0;
  for (const auto& value : values)
    if (value.id == std::numeric_limits<std::uint32_t>::max())
      throw std::overflow_error("constant ID space exhausted");
    else
      next = std::max(next, value.id + 1);
  return next;
}
std::uint32_t nextNodeId(std::span<const ir::Node> values) {
  std::uint32_t next = 0;
  for (const auto& value : values)
    next = std::max(next, value.id == std::numeric_limits<std::uint32_t>::max()
                              ? throw std::overflow_error("node ID space exhausted")
                              : value.id + 1);
  return next;
}
} // namespace

std::string_view toString(KernelABIBindingStatus status) noexcept {
  switch (status) {
  case KernelABIBindingStatus::Success:
    return "success";
  case KernelABIBindingStatus::InvalidSourceDFG:
    return "invalid_source_dfg";
  case KernelABIBindingStatus::InvalidInvocation:
    return "invalid_invocation";
  case KernelABIBindingStatus::UnsupportedInputType:
    return "unsupported_input_type";
  case KernelABIBindingStatus::UnsupportedLiveOutType:
    return "unsupported_liveout_type";
  case KernelABIBindingStatus::ScratchpadABIRegionConflict:
    return "scratchpad_abi_region_conflict";
  case KernelABIBindingStatus::ScratchpadABICapacityExceeded:
    return "scratchpad_abi_capacity_exceeded";
  case KernelABIBindingStatus::BindingFailure:
    return "binding_failure";
  case KernelABIBindingStatus::VerificationFailure:
    return "verification_failure";
  }
  return "binding_failure";
}

std::string KernelABIBindingResult::format() const {
  std::ostringstream out;
  out << toString(status);
  for (const auto& diagnostic : diagnostics)
    out << '\n' << diagnostic.code << ": " << diagnostic.message;
  return out.str();
}

std::string KernelABIBindingResult::toJson() const {
  nlohmann::json root = {{"schema", "cgra.kernel_abi.binding.v1"},
                         {"status", toString(status)},
                         {"diagnostics", nlohmann::json::array()}};
  for (const auto& diagnostic : diagnostics)
    root["diagnostics"].push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
  return root.dump(2) + "\n";
}

KernelABIBindingResult KernelABIBinder::bind(const ir::DFG& source, const TargetModel& target,
                                             const KernelInvocation& invocation,
                                             std::string kernelName) {
  KernelABIBindingResult result;
  const auto sourceReport = ir::DFGVerifier::verify(source);
  if (!sourceReport.ok()) {
    result.status = KernelABIBindingStatus::InvalidSourceDFG;
    add(result, "ABI_INVALID_SOURCE_DFG", sourceReport.format());
    return result;
  }
  KernelSignature signature = inferSignature(source);
  if (!kernelName.empty())
    signature.kernelName = std::move(kernelName);
  std::set<std::string> inputNames;
  for (const auto& input : signature.scalarInputs)
    if (!inputNames.insert(input.name).second) {
      result.status = KernelABIBindingStatus::InvalidSourceDFG;
      add(result, "ABI_AMBIGUOUS_INPUT_NAME", "external value names must be unique");
      return result;
    }
  const auto depth = target.memory().depth;
  const auto invocationReport =
      validateInvocation(signature, invocation, depth, target.memory().widthBits);
  if (!invocationReport.ok()) {
    result.status = KernelABIBindingStatus::InvalidInvocation;
    add(result, "ABI_INVALID_INVOCATION", invocationReport.message);
    return result;
  }
  const auto outputCount = source.liveOuts().size();
  if (outputCount > depth) {
    result.status = KernelABIBindingStatus::ScratchpadABICapacityExceeded;
    add(result, "ABI_OUTPUT_REGION_TOO_SMALL", "live-outs exceed target scratchpad depth");
    return result;
  }
  const auto outputBase = depth - static_cast<std::uint32_t>(outputCount);
  for (const auto& [address, value] : invocation.scratchpadPreload) {
    (void)value;
    if (address >= outputBase && outputCount != 0) {
      result.status = KernelABIBindingStatus::ScratchpadABIRegionConflict;
      add(result, "ABI_SCRATCHPAD_PRELOAD_COLLISION",
          "scratchpad preload overlaps ABI output region");
      return result;
    }
  }
  if (outputCount != 0) {
    for (const auto& binding : source.externalBindings()) {
      if (!std::holds_alternative<ir::ConstantRef>(binding.source) || binding.operand != 0)
        continue;
      const auto& node = source.node(binding.node);
      if (node.opcode != ir::Opcode::Load && node.opcode != ir::Opcode::Store)
        continue;
      const auto address = source.constant(std::get<ir::ConstantRef>(binding.source).value).bits;
      if (address >= outputBase && address < depth) {
        result.status = KernelABIBindingStatus::ScratchpadABIRegionConflict;
        add(result, "ABI_CONSTANT_ADDRESS_COLLISION",
            "constant memory address operand overlaps the ABI output region");
        return result;
      }
    }
  }
  try {
    ir::DFGBuilder builder(source.name());
    for (const auto& value : source.externalValues())
      builder.importExternal(value);
    for (const auto& value : source.constants())
      builder.importConstant(value);
    for (const auto& node : source.nodes())
      builder.importNode(node);
    for (const auto& output : source.liveOuts())
      builder.importLiveOut(output);

    const auto constantStart = nextId(source.constants());
    std::map<ir::ExternalValueId, ir::ConstantId> specialized;
    for (const auto& desc : signature.scalarInputs) {
      const auto input =
          std::find_if(invocation.scalarInputs.begin(), invocation.scalarInputs.end(),
                       [&](const auto& value) { return value.id == desc.id; });
      if (input == invocation.scalarInputs.end())
        throw std::invalid_argument("validated invocation lost a scalar input");
      const auto id = constantStart + static_cast<ir::ConstantId>(specialized.size());
      builder.importConstant({id, desc.type, input->bits});
      specialized.emplace(desc.id, id);
    }

    for (const auto& edge : source.edges()) {
      ir::Edge copy = edge;
      auto rewriteBoundary = [&](auto& info) {
        if (!info.boundary)
          return;
        for (auto& item : info.boundary->values) {
          if (std::holds_alternative<ir::ExternalValueRef>(item.value)) {
            const auto external = std::get<ir::ExternalValueRef>(item.value).value;
            const auto it = specialized.find(external);
            if (it == specialized.end())
              throw std::invalid_argument("boundary references an unknown specialized external");
            item.value = ir::ConstantRef{it->second};
          }
        }
      };
      if (copy.kind() == ir::Edge::Kind::Data)
        rewriteBoundary(std::get<ir::DataEdgeInfo>(copy.info));
      else if (copy.kind() == ir::Edge::Kind::Predicate)
        rewriteBoundary(std::get<ir::PredicateEdgeInfo>(copy.info));
      builder.importEdge(std::move(copy));
    }
    for (const auto& binding : source.externalBindings()) {
      if (std::holds_alternative<ir::ExternalValueRef>(binding.source)) {
        const auto external = std::get<ir::ExternalValueRef>(binding.source).value;
        builder.bindConstant(binding.node, binding.operand, specialized.at(external));
      } else {
        builder.bindConstant(binding.node, binding.operand,
                             std::get<ir::ConstantRef>(binding.source).value);
      }
    }

    KernelABILayout layout{depth, outputBase, outputBase, {}, {}};
    for (const auto& [external, constant] : specialized)
      layout.inputs.push_back({external, constant});

    auto nextNode = nextNodeId(source.nodes());
    auto nextConstant = constantStart + static_cast<ir::ConstantId>(specialized.size());
    for (const auto& output : source.liveOuts()) {
      if (output.type.kind != ir::ValueKind::Integer ||
          output.type.bitWidth != target.memory().widthBits) {
        result.status = KernelABIBindingStatus::UnsupportedLiveOutType;
        add(result, "ABI_UNSUPPORTED_LIVEOUT_TYPE", "V0 supports only i32 data live-outs");
        return result;
      }
      const auto ordinal = static_cast<std::uint32_t>(layout.outputs.size());
      const auto address = outputBase + ordinal;
      const auto addressConstant = nextConstant++;
      const auto storeNode = nextNode++;
      builder.importConstant({addressConstant, ir::ValueType::i32(), address});
      builder.importNode({storeNode,
                          ir::Opcode::Store,
                          ir::ValueType::voidTy(),
                          {ir::ValueType::i32(), output.type},
                          std::nullopt,
                          ir::MemoryOpInfo{target.memory().widthBits, false},
                          ir::SourceInfo{"abi.liveout." + std::to_string(output.id)}});
      builder.bindConstant(storeNode, 0, addressConstant);
      builder.addDataEdge(output.source, storeNode, 1);
      builder.addMemoryEdge(storeNode, storeNode, ir::MemoryDepKind::WAW, 1);
      layout.outputs.push_back({output.id, address, storeNode, addressConstant});
    }
    ABIBoundKernel bound{builder.finish(), signature, invocation, layout};
    const auto verification = KernelABIVerifier::verify(source, target, invocation, bound);
    if (!verification.ok()) {
      result.status = KernelABIBindingStatus::VerificationFailure;
      add(result, "ABI_FINAL_VERIFICATION_FAILED", verification.format());
      return result;
    }
    result.status = KernelABIBindingStatus::Success;
    result.bound = std::move(bound);
    return result;
  } catch (const std::exception& error) {
    result.status = KernelABIBindingStatus::BindingFailure;
    add(result, "ABI_BINDING_FAILED", error.what());
    return result;
  }
}
} // namespace cgra::abi
