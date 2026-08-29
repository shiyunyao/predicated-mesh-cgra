// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Target/TargetLegalizer.h"
#include "cgra/Target/TargetModel.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef CGRA_REPOSITORY_ROOT
#define CGRA_REPOSITORY_ROOT "."
#endif

namespace {
void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::ir::DFG singleAdd() {
  cgra::ir::DFGBuilder builder("feasibility_fallback");
  const auto lhs = builder.addExternal("lhs", cgra::ir::ValueType::i32());
  const auto rhs = builder.addExternal("rhs", cgra::ir::ValueType::i32());
  const auto add = builder.addNode(cgra::ir::Opcode::Add,
                                   {cgra::ir::ValueType::i32(), cgra::ir::ValueType::i32()},
                                   cgra::ir::ValueType::i32());
  builder.bindExternal(add, 0, lhs);
  builder.bindExternal(add, 1, rhs);
  builder.addLiveOut("sum", cgra::ir::ValueType::i32(), add);
  return builder.finish();
}
} // namespace

int main() {
  try {
    const auto root = std::filesystem::path(CGRA_REPOSITORY_ROOT);
    const auto target = cgra::TargetModel::loadFromFile(root / "target/cgra_v3.json");
    const auto legalized = cgra::target::TargetLegalizer::legalize(singleAdd(), target);
    expect(legalized.ok(), "simple Add must legalize");

    cgra::mapping::ModuloMapperOptions options;
    options.maxII = 2;
    options.objective = cgra::mapping::MappingObjective::FindAnyFeasible;
    options.feasibilityFallback.enabled = true;
    options.feasibilityFallback.lowIIWindow = 0;
    options.completeMappingChecker = [](const cgra::target::TargetDFG&, const cgra::TargetModel&,
                                        const cgra::mapping::ModuloMapping& mapping) {
      if (mapping.ii() == 1)
        return cgra::mapping::CompleteMappingCheckResult{
            cgra::mapping::CompleteMappingDecision::Reject,
            "rf_fixed_register_self_overlap", "synthetic finite-RF rejection"};
      return cgra::mapping::CompleteMappingCheckResult{
          cgra::mapping::CompleteMappingDecision::Accept, "", ""};
    };
    const auto mapped = cgra::mapping::ModuloMapper::map(*legalized.dfg, target, options);
    expect(mapped.ok(), "fallback must continue after a rejected completed mapping");
    expect(mapped.mii == 1 && mapped.safeII == 2 && mapped.bestKnownII == 2,
           "fallback must report the accepted II separately from MII");
    expect(mapped.solutionKind == "constructive_fallback" && mapped.fallbackInvoked,
           "find-any-feasible must use the independent constructive scheduler");
    expect(mapped.fallbackAttempts >= 1,
           "constructive fallback must report the II attempts it performed");
    expect(mapped.stats.completedModuloMappings >= 2 && mapped.stats.rfRejected >= 1,
           "raw candidates and finite-RF rejection telemetry must both be retained");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
