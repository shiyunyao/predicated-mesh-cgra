// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelInvocation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cgra::abi {

struct KernelInputBinding {
  ir::ExternalValueId input = 0;
  ir::ConstantId specializedConstant = 0;
  friend bool operator==(const KernelInputBinding&, const KernelInputBinding&) = default;
};

struct KernelOutputBinding {
  ir::LiveOutId output = 0;
  std::uint32_t scratchpadAddress = 0;
  ir::NodeId abiStoreNode = 0;
  ir::ConstantId addressConstant = 0;
  friend bool operator==(const KernelOutputBinding&, const KernelOutputBinding&) = default;
};

struct KernelABILayout {
  std::uint32_t scratchpadDepth = 0;
  std::uint32_t userScratchpadLimit = 0;
  std::uint32_t outputRegionBase = 0;
  std::vector<KernelInputBinding> inputs;
  std::vector<KernelOutputBinding> outputs;
};

struct ABIBoundKernel {
  ir::DFG dfg;
  KernelSignature signature;
  KernelInvocation invocation;
  KernelABILayout layout;
};

std::string toJson(const KernelABILayout& layout, const KernelSignature* signature = nullptr,
                   const KernelInvocation* invocation = nullptr);

} // namespace cgra::abi
