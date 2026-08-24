// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/ABI/KernelABILayout.h"
#include "cgra/Target/TargetModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgra::abi {

enum class KernelABIBindingStatus {
  Success,
  InvalidSourceDFG,
  InvalidInvocation,
  UnsupportedInputType,
  UnsupportedLiveOutType,
  ScratchpadABIRegionConflict,
  ScratchpadABICapacityExceeded,
  BindingFailure,
  VerificationFailure,
};

std::string_view toString(KernelABIBindingStatus status) noexcept;

struct KernelABIBindingDiagnostic {
  std::string code;
  std::string message;
  std::optional<ir::ExternalValueId> input;
  std::optional<ir::LiveOutId> output;
  std::optional<ir::EdgeId> edge;
};

struct KernelABIBindingResult {
  KernelABIBindingStatus status = KernelABIBindingStatus::BindingFailure;
  std::optional<ABIBoundKernel> bound;
  std::vector<KernelABIBindingDiagnostic> diagnostics;
  bool ok() const noexcept {
    return status == KernelABIBindingStatus::Success && bound.has_value();
  }
  std::string format() const;
  std::string toJson() const;
};

class KernelABIBinder {
public:
  static KernelABIBindingResult bind(const ir::DFG& source, const TargetModel& target,
                                     const KernelInvocation& invocation,
                                     std::string kernelName = {});
};

} // namespace cgra::abi
