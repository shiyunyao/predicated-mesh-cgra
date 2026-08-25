// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <string>
#include <vector>

namespace cgra::abi {

struct KernelInputDesc {
  ir::ExternalValueId id = 0;
  std::string name;
  ir::ValueType type = ir::ValueType::voidTy();
  friend bool operator==(const KernelInputDesc&, const KernelInputDesc&) = default;
};

struct KernelOutputDesc {
  ir::LiveOutId id = 0;
  std::string name;
  ir::ValueType type = ir::ValueType::voidTy();
  ir::NodeId source = 0;
  friend bool operator==(const KernelOutputDesc&, const KernelOutputDesc&) = default;
};

struct KernelSignature {
  std::string kernelName;
  std::vector<KernelInputDesc> scalarInputs;
  std::vector<KernelOutputDesc> scalarOutputs;
  bool requiresTripCount = true;
  friend bool operator==(const KernelSignature&, const KernelSignature&) = default;
};

KernelSignature inferSignature(const ir::DFG& dfg);
std::string toJson(const KernelSignature& signature);

} // namespace cgra::abi
