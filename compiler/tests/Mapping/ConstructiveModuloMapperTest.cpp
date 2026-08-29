// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGBuilder.h"
#include "cgra/Mapping/ConstructiveModuloMapper.h"
#include "cgra/Pipeline/BackendFeasibilityChecker.h"
#include "cgra/Target/TargetDFGVerifier.h"
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

cgra::ir::DFG simpleAdd() {
  cgra::ir::DFGBuilder builder("constructive_simple_add");
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
    const auto legalized = cgra::target::TargetLegalizer::legalize(simpleAdd(), target);
    expect(legalized.ok(), "simple add must target-legalize");
    const auto targetReport = cgra::target::TargetDFGVerifier::verify(*legalized.dfg, target);
    expect(targetReport.ok(), "legalized DFG must verify");

    cgra::pipeline::BackendFeasibilityChecker checker;
    cgra::mapping::ConstructiveModuloMapperOptions options;
    options.minII = 1;
    options.maxSafeII = 4;
    options.completeMappingChecker = [&checker](const cgra::target::TargetDFG& dfg,
                                                 const cgra::TargetModel& model,
                                                 const cgra::mapping::ModuloMapping& mapping) {
      return checker.check(dfg, model, mapping);
    };

    const auto result = cgra::mapping::mapConstructively(*legalized.dfg, target, options);
    expect(result.ok(), "constructive mapper must pass the real stage/RF checker");
    expect(result.solutionKind == "constructive_fallback", "solution kind must identify fallback");
    expect(result.safeII >= result.mii && result.safeII <= options.maxSafeII,
           "safe II must be within the declared constructive range");
    expect(result.stats.completedModuloMappings > 0, "a verified modulo candidate is required");
    std::cout << "CONSTRUCTIVE_MODULO_MAPPER_PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CONSTRUCTIVE_MODULO_MAPPER_FAIL: " << error.what() << '\n';
    return 1;
  }
}
