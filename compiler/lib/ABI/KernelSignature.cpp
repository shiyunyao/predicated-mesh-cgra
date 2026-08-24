// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelSignature.h"

#include <nlohmann/json.hpp>

namespace cgra::abi {
namespace {
nlohmann::json typeJson(const ir::ValueType& type) {
  return {{"kind", ir::toString(type.kind)}, {"bits", type.bitWidth}};
}
} // namespace

KernelSignature inferSignature(const ir::DFG& dfg) {
  KernelSignature result;
  result.kernelName = dfg.name();
  for (const auto& value : dfg.externalValues())
    result.scalarInputs.push_back({value.id, value.name, value.type});
  for (const auto& value : dfg.liveOuts())
    result.scalarOutputs.push_back({value.id, value.name, value.type, value.source});
  return result;
}

std::string toJson(const KernelSignature& signature) {
  nlohmann::json root = {{"schema", "cgra.kernel_signature.v1"},
                         {"kernel", signature.kernelName},
                         {"requires_trip_count", signature.requiresTripCount},
                         {"inputs", nlohmann::json::array()},
                         {"outputs", nlohmann::json::array()}};
  for (const auto& input : signature.scalarInputs)
    root["inputs"].push_back(
        {{"id", input.id}, {"name", input.name}, {"type", typeJson(input.type)}});
  for (const auto& output : signature.scalarOutputs)
    root["outputs"].push_back({{"id", output.id},
                               {"name", output.name},
                               {"type", typeJson(output.type)},
                               {"source", output.source}});
  return root.dump(2) + "\n";
}
} // namespace cgra::abi
