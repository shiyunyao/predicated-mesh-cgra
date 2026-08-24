// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelABIVerifier.h"

#include "cgra/IR/DFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace cgra::abi {
namespace {
void fail(KernelABIVerificationReport& report, std::string message) {
  report.diagnostics.push_back(std::move(message));
}
const KernelInputBinding* inputBinding(const KernelABILayout& layout, ir::ExternalValueId id) {
  const auto it = std::find_if(layout.inputs.begin(), layout.inputs.end(),
                               [&](const auto& item) { return item.input == id; });
  return it == layout.inputs.end() ? nullptr : &*it;
}
const KernelOutputBinding* outputBinding(const KernelABILayout& layout, ir::LiveOutId id) {
  const auto it = std::find_if(layout.outputs.begin(), layout.outputs.end(),
                               [&](const auto& item) { return item.output == id; });
  return it == layout.outputs.end() ? nullptr : &*it;
}
bool sameBoundary(const std::optional<ir::RecurrenceBoundary>& lhs,
                  const std::optional<ir::RecurrenceBoundary>& rhs,
                  const std::map<ir::ExternalValueId, ir::ConstantId>& specialized) {
  if (lhs.has_value() != rhs.has_value())
    return false;
  if (!lhs)
    return true;
  if (lhs->values.size() != rhs->values.size())
    return false;
  for (std::size_t i = 0; i < lhs->values.size(); ++i) {
    auto expected = lhs->values[i].value;
    if (std::holds_alternative<ir::ExternalValueRef>(expected)) {
      const auto external = std::get<ir::ExternalValueRef>(expected).value;
      const auto it = specialized.find(external);
      if (it == specialized.end())
        return false;
      expected = ir::ConstantRef{it->second};
    }
    if (lhs->values[i].iterationOffset != rhs->values[i].iterationOffset ||
        expected != rhs->values[i].value)
      return false;
  }
  return true;
}
} // namespace

std::string KernelABIVerificationReport::format() const {
  std::string result = valid ? "valid kernel ABI binding" : "invalid kernel ABI binding";
  for (const auto& diagnostic : diagnostics)
    result += "\nABI: " + diagnostic;
  return result;
}

std::string KernelABIVerificationReport::toJson() const {
  nlohmann::json root = {{"schema", "cgra.kernel_abi.verification.v1"},
                         {"valid", valid},
                         {"diagnostics", diagnostics}};
  return root.dump(2) + "\n";
}

KernelABIVerificationReport KernelABIVerifier::verify(const ir::DFG& source,
                                                      const TargetModel& target,
                                                      const KernelInvocation& invocation,
                                                      const ABIBoundKernel& bound) {
  KernelABIVerificationReport report;
  const auto sourceReport = ir::DFGVerifier::verify(source);
  const auto boundReport = ir::DFGVerifier::verify(bound.dfg);
  if (!sourceReport.ok())
    fail(report, "source DFG is invalid");
  if (!boundReport.ok())
    fail(report, boundReport.format());
  auto expectedSignature = inferSignature(source);
  expectedSignature.kernelName = bound.signature.kernelName;
  if (!(bound.signature == expectedSignature))
    fail(report, "kernel signature does not match source DFG");
  const auto invocationReport = validateInvocation(
      bound.signature, invocation, target.memory().depth, target.memory().widthBits);
  if (!invocationReport.ok())
    fail(report, invocationReport.message);
  if (bound.invocation.tripCount != invocation.tripCount)
    fail(report, "bound invocation trip count differs from requested invocation");
  if (bound.invocation.scalarInputs != invocation.scalarInputs)
    fail(report, "bound invocation scalar inputs differ from requested invocation");
  if (bound.invocation.scratchpadPreload != invocation.scratchpadPreload)
    fail(report, "bound invocation scratchpad preload differs from requested invocation");
  if (bound.dfg.name() != source.name())
    fail(report, "bound DFG name changed");
  for (const auto& node : source.nodes())
    if (!bound.dfg.containsNode(node.id) || bound.dfg.node(node.id) != node)
      fail(report, "original node was changed or removed");
  for (const auto& value : source.externalValues())
    if (!bound.dfg.containsExternal(value.id) || bound.dfg.externalValue(value.id) != value)
      fail(report, "original external value was changed or removed");
  for (const auto& value : source.constants())
    if (!bound.dfg.containsConstant(value.id) || bound.dfg.constant(value.id) != value)
      fail(report, "original constant was changed or removed");
  for (const auto& output : source.liveOuts())
    if (!bound.dfg.containsLiveOut(output.id) || bound.dfg.liveOut(output.id) != output)
      fail(report, "original live-out was changed or removed");

  std::map<ir::ExternalValueId, ir::ConstantId> inputs;
  for (const auto& input : bound.layout.inputs) {
    if (inputs.contains(input.input))
      fail(report, "duplicate ABI input binding");
    inputs[input.input] = input.specializedConstant;
    if (!bound.dfg.containsConstant(input.specializedConstant))
      fail(report, "specialized input constant is missing");
    else {
      const auto invocationValue =
          std::find_if(invocation.scalarInputs.begin(), invocation.scalarInputs.end(),
                       [&](const auto& item) { return item.id == input.input; });
      if (invocationValue == invocation.scalarInputs.end() ||
          bound.dfg.constant(input.specializedConstant).bits != invocationValue->bits)
        fail(report, "specialized input constant has the wrong invocation value");
    }
  }
  std::set<ir::ExternalValueId> sourceInputIds;
  for (const auto& value : source.externalValues())
    sourceInputIds.insert(value.id);
  if (inputs.size() != sourceInputIds.size())
    fail(report, "not every source external has an ABI binding");
  for (const auto& [id, constant] : inputs) {
    (void)constant;
    if (!sourceInputIds.contains(id))
      fail(report, "ABI input binding references an unknown source external");
  }
  for (const auto& binding : source.externalBindings()) {
    if (std::holds_alternative<ir::ConstantRef>(binding.source)) {
      const auto it =
          std::find_if(bound.dfg.externalBindings().begin(), bound.dfg.externalBindings().end(),
                       [&](const auto& candidate) { return candidate == binding; });
      if (it == bound.dfg.externalBindings().end())
        fail(report, "original constant use was changed");
      continue;
    }
    const auto* found =
        inputBinding(bound.layout, std::get<ir::ExternalValueRef>(binding.source).value);
    if (!found)
      fail(report, "ordinary external use has no ABI binding");
    const auto it = std::find_if(bound.dfg.externalBindings().begin(),
                                 bound.dfg.externalBindings().end(), [&](const auto& candidate) {
                                   return candidate.node == binding.node &&
                                          candidate.operand == binding.operand;
                                 });
    if (it == bound.dfg.externalBindings().end() ||
        !std::holds_alternative<ir::ConstantRef>(it->source) ||
        std::get<ir::ConstantRef>(it->source).value != found->specializedConstant)
      fail(report, "ordinary external use was not rewritten to its specialized constant");
  }
  for (const auto& edge : source.edges()) {
    if (!bound.dfg.containsEdge(edge.id)) {
      fail(report, "original edge was removed");
      continue;
    }
    const auto& actual = bound.dfg.edge(edge.id);
    if (actual.src != edge.src || actual.dst != edge.dst || actual.distance != edge.distance ||
        actual.kind() != edge.kind())
      fail(report, "original edge topology changed");
    if (edge.kind() == ir::Edge::Kind::Data) {
      const auto& expected = std::get<ir::DataEdgeInfo>(edge.info);
      const auto& got = std::get<ir::DataEdgeInfo>(actual.info);
      if (expected.dstOperand != got.dstOperand ||
          !sameBoundary(expected.boundary, got.boundary, inputs))
        fail(report, "data edge semantics changed");
    } else if (edge.kind() == ir::Edge::Kind::Predicate) {
      const auto& expected = std::get<ir::PredicateEdgeInfo>(edge.info);
      const auto& got = std::get<ir::PredicateEdgeInfo>(actual.info);
      if (expected.dstOperand != got.dstOperand ||
          !sameBoundary(expected.boundary, got.boundary, inputs))
        fail(report, "predicate edge semantics changed");
    } else if (std::get<ir::MemoryEdgeInfo>(edge.info).dependence !=
               std::get<ir::MemoryEdgeInfo>(actual.info).dependence) {
      fail(report, "memory edge dependence changed");
    }
  }
  std::set<std::uint32_t> outputAddresses;
  if (bound.layout.outputs.size() != source.liveOuts().size())
    fail(report, "ABI layout does not contain exactly one output binding per source live-out");
  std::set<ir::LiveOutId> outputIds;
  for (const auto& item : bound.layout.outputs) {
    if (!outputIds.insert(item.output).second || !source.containsLiveOut(item.output))
      fail(report, "ABI output binding has a duplicate or unknown live-out ID");
  }
  if (bound.layout.outputRegionBase != bound.layout.scratchpadDepth - bound.layout.outputs.size())
    fail(report, "ABI output region is not at the top of scratchpad");
  for (const auto& output : source.liveOuts()) {
    const auto* binding = outputBinding(bound.layout, output.id);
    if (!binding) {
      fail(report, "live-out has no ABI output binding");
      continue;
    }
    if (!outputAddresses.insert(binding->scratchpadAddress).second ||
        binding->scratchpadAddress >= bound.layout.scratchpadDepth ||
        binding->scratchpadAddress < bound.layout.outputRegionBase)
      fail(report, "ABI output address is duplicate or outside reserved region");
    if (!bound.dfg.containsNode(binding->abiStoreNode)) {
      fail(report, "ABI live-out Store is missing");
      continue;
    }
    const auto& store = bound.dfg.node(binding->abiStoreNode);
    if (store.opcode != ir::Opcode::Store || !store.source ||
        store.source->label != "abi.liveout." + std::to_string(output.id))
      fail(report, "ABI output node is not a provenance-marked Store");
    bool addressOk = false, dataOk = false, wawOk = false;
    for (const auto& item : bound.dfg.externalBindings())
      if (item.node == store.id && item.operand == 0 &&
          std::holds_alternative<ir::ConstantRef>(item.source) &&
          std::get<ir::ConstantRef>(item.source).value == binding->addressConstant)
        addressOk = true;
    for (const auto edgeId : bound.dfg.incoming(store.id)) {
      const auto& edge = bound.dfg.edge(edgeId);
      if (edge.kind() == ir::Edge::Kind::Data && edge.src == output.source &&
          std::get<ir::DataEdgeInfo>(edge.info).dstOperand == 1)
        dataOk = true;
    }
    for (const auto edgeId : bound.dfg.outgoing(store.id)) {
      const auto& edge = bound.dfg.edge(edgeId);
      if (edge.kind() == ir::Edge::Kind::Memory && edge.src == store.id && edge.dst == store.id &&
          edge.distance == 1 &&
          std::get<ir::MemoryEdgeInfo>(edge.info).dependence == ir::MemoryDepKind::WAW)
        wawOk = true;
    }
    if (!addressOk || !dataOk || !wawOk)
      fail(report, "ABI output Store has incorrect address, value edge, or self-WAW");
  }
  for (const auto& [address, value] : invocation.scratchpadPreload) {
    (void)value;
    if (address >= bound.layout.outputRegionBase && !bound.layout.outputs.empty())
      fail(report, "scratchpad preload collides with ABI output region");
  }
  report.valid = report.diagnostics.empty();
  return report;
}
} // namespace cgra::abi
