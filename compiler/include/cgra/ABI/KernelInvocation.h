// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelSignature.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cgra::abi {

struct KernelScalarInputValue {
  ir::ExternalValueId id = 0;
  std::uint64_t bits = 0;
  friend bool operator==(const KernelScalarInputValue&, const KernelScalarInputValue&) = default;
};

struct KernelInvocation {
  std::uint64_t tripCount = 0;
  std::vector<KernelScalarInputValue> scalarInputs;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> scratchpadPreload;
};

struct KernelInvocationValidationResult {
  bool valid = false;
  std::string message;
  bool ok() const noexcept { return valid; }
};

KernelInvocationValidationResult validateInvocation(const KernelSignature& signature,
                                                    const KernelInvocation& invocation,
                                                    std::uint32_t scratchpadDepth,
                                                    std::uint16_t scalarWidthBits = 32);
KernelInvocation parseInvocation(std::string_view jsonText, const KernelSignature& signature);
std::string toJson(const KernelInvocation& invocation, const KernelSignature* signature = nullptr);

} // namespace cgra::abi
