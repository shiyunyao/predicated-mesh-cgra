// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFGVerifier.h"
#include "cgra/Target/TargetDFG.h"
#include "cgra/Target/TargetModel.h"

#include <optional>
#include <string>
#include <vector>

namespace cgra::target {

enum class LegalizationStatus {
  Success,
  InvalidGenericDFG,
  UnsupportedType,
  UnsupportedOperation,
  UnsupportedComparePredicate,
  UnsupportedMemoryAccessWidth,
  UnsupportedAddressType,
  UnsupportedMemoryAlignment,
  NoCompatibleExecutionResource,
  TargetContractError,
  InternalError,
};

std::string_view toString(LegalizationStatus status);

struct LegalizationDiagnostic {
  LegalizationStatus status = LegalizationStatus::InternalError;
  std::string code;
  std::string message;
  std::optional<ir::NodeId> genericNode;
};

struct TargetLegalizationResult {
  std::optional<TargetDFG> dfg;
  LegalizationMap map;
  std::vector<LegalizationDiagnostic> diagnostics;

  bool ok() const noexcept { return dfg.has_value() && diagnostics.empty(); }
  std::string format() const;
  std::string toJson() const;
};

class TargetLegalizer {
public:
  static TargetLegalizationResult legalize(const ir::DFG& generic, const TargetModel& target);
};

} // namespace cgra::target
