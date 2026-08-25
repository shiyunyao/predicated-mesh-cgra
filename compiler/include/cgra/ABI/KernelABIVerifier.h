// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelABILayout.h"
#include "cgra/Target/TargetModel.h"

#include <string>
#include <vector>

namespace cgra::abi {

struct KernelABIVerificationReport {
  bool valid = false;
  std::vector<std::string> diagnostics;
  bool ok() const noexcept { return valid; }
  std::string format() const;
  std::string toJson() const;
};

class KernelABIVerifier {
public:
  static KernelABIVerificationReport verify(const ir::DFG& source, const TargetModel& target,
                                            const KernelInvocation& invocation,
                                            const ABIBoundKernel& bound);
};

} // namespace cgra::abi
