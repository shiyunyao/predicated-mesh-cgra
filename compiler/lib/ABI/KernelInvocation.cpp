// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelInvocation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace cgra::abi {
namespace {
std::uint64_t parseBits(const nlohmann::json& value) {
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto signedValue = value.get<std::int64_t>();
    if (signedValue < 0)
      throw std::invalid_argument("kernel scalar input must be an unsigned bit pattern");
    return static_cast<std::uint64_t>(signedValue);
  }
  if (!value.is_string())
    throw std::invalid_argument("kernel scalar input must be an integer or hexadecimal string");
  const auto text = value.get<std::string>();
  std::uint64_t result = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const int base = text.starts_with("0x") || text.starts_with("0X") ? 16 : 10;
  if (base == 16)
    begin += 2;
  const auto parsed = std::from_chars(begin, end, result, base);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("invalid kernel scalar bit pattern: " + text);
  return result;
}
} // namespace

KernelInvocationValidationResult validateInvocation(const KernelSignature& signature,
                                                    const KernelInvocation& invocation,
                                                    std::uint32_t scratchpadDepth,
                                                    std::uint16_t scalarWidthBits) {
  if (invocation.tripCount == 0)
    return {false, "ABI_INVALID_TRIP_COUNT: trip count must be positive"};
  std::set<ir::ExternalValueId> seen;
  for (const auto& input : invocation.scalarInputs) {
    if (!seen.insert(input.id).second)
      return {false, "ABI_DUPLICATE_INPUT: duplicate scalar input"};
    const auto it = std::find_if(signature.scalarInputs.begin(), signature.scalarInputs.end(),
                                 [&](const auto& desc) { return desc.id == input.id; });
    if (it == signature.scalarInputs.end())
      return {false, "ABI_UNKNOWN_INPUT: scalar input is not in the kernel signature"};
    if (it->type.bitWidth < 64 && (input.bits >> it->type.bitWidth) != 0)
      return {false, "ABI_INPUT_TYPE_MISMATCH: scalar value does not fit its declared width"};
    if (it->type.kind != ir::ValueKind::Integer && it->type.kind != ir::ValueKind::Predicate)
      return {false, "ABI_UNSUPPORTED_INPUT_TYPE: only integer and predicate inputs are supported"};
    if (it->type.kind == ir::ValueKind::Integer && it->type.bitWidth != scalarWidthBits)
      return {false, "ABI_UNSUPPORTED_INPUT_TYPE: scalar width does not match target data width"};
    if (it->type.kind == ir::ValueKind::Predicate && input.bits > 1)
      return {false, "ABI_INPUT_TYPE_MISMATCH: predicate input must be zero or one"};
  }
  if (seen.size() != signature.scalarInputs.size())
    return {false, "ABI_MISSING_INPUT: invocation does not bind every scalar input"};
  std::set<std::uint32_t> preloadAddresses;
  for (const auto& [address, value] : invocation.scratchpadPreload) {
    const auto preloadAddress = address;
    const auto preloadValue = value;
    if (address >= scratchpadDepth)
      return {false, "ABI_SCRATCHPAD_ADDRESS_OUT_OF_RANGE: preload address exceeds target SPM"};
    if (!preloadAddresses.insert(address).second) {
      const auto duplicate =
          std::find_if(invocation.scratchpadPreload.begin(), invocation.scratchpadPreload.end(),
                       [preloadAddress, preloadValue](const auto& item) {
                         return item.first == preloadAddress && item.second != preloadValue;
                       });
      if (duplicate != invocation.scratchpadPreload.end())
        return {false, "ABI_DUPLICATE_PRELOAD: address has conflicting values"};
    }
  }
  return {true, {}};
}

KernelInvocation parseInvocation(std::string_view jsonText, const KernelSignature& signature) {
  const auto root = nlohmann::json::parse(jsonText);
  if (!root.is_object() || root.value("schema", "") != "cgra.kernel_invocation.v1")
    throw std::invalid_argument("invocation JSON schema must be cgra.kernel_invocation.v1");
  KernelInvocation result;
  result.tripCount = root.at("trip_count").get<std::uint64_t>();
  if (root.contains("scalar_inputs")) {
    const auto& inputs = root.at("scalar_inputs");
    if (!inputs.is_object())
      throw std::invalid_argument("scalar_inputs must be an object keyed by input name");
    for (const auto& [name, value] : inputs.items()) {
      const auto inputName = name;
      const auto it =
          std::find_if(signature.scalarInputs.begin(), signature.scalarInputs.end(),
                       [inputName](const auto& desc) { return desc.name == inputName; });
      if (it == signature.scalarInputs.end())
        throw std::invalid_argument("unknown kernel input name: " + name);
      result.scalarInputs.push_back({it->id, parseBits(value)});
    }
  }
  if (root.contains("scratchpad_preload")) {
    for (const auto& item : root.at("scratchpad_preload"))
      result.scratchpadPreload.emplace_back(
          item.at("address").get<std::uint32_t>(),
          static_cast<std::uint32_t>(parseBits(item.at("value"))));
  }
  return result;
}

std::string toJson(const KernelInvocation& invocation, const KernelSignature* signature) {
  nlohmann::json root = {{"schema", "cgra.kernel_invocation.v1"},
                         {"trip_count", invocation.tripCount},
                         {"scalar_inputs", nlohmann::json::object()},
                         {"scratchpad_preload", nlohmann::json::array()}};
  for (const auto& input : invocation.scalarInputs) {
    std::string name = std::to_string(input.id);
    if (signature) {
      const auto it = std::find_if(signature->scalarInputs.begin(), signature->scalarInputs.end(),
                                   [&](const auto& desc) { return desc.id == input.id; });
      if (it != signature->scalarInputs.end())
        name = it->name;
    }
    root["scalar_inputs"][name] = "0x" + [&] {
      char buffer[17] = {};
      std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(input.bits));
      return std::string(buffer);
    }();
  }
  for (const auto& [address, value] : invocation.scratchpadPreload)
    root["scratchpad_preload"].push_back({{"address", address}, {"value", value}});
  return root.dump(2) + "\n";
}
} // namespace cgra::abi
