// SPDX-License-Identifier: MIT
#include "cgra/Lowering/TargetLowering.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/Schedule/MaterializedScheduleSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 7) {
    std::cerr << "usage: cgra-target-lower target_dfg.json rf_mapping.json "
                 "materialized_schedule.json --target target.json -o manifest.json\n";
    return 2;
  }
  try {
    std::string dfgPath = argv[1];
    std::string rfPath = argv[2];
    std::string schedulePath = argv[3];
    std::string targetPath;
    std::string outputPath;
    for (int index = 4; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--target" && index + 1 < argc)
        targetPath = argv[++index];
      else if (argument == "-o" && index + 1 < argc)
        outputPath = argv[++index];
    }
    if (targetPath.empty() || outputPath.empty())
      throw std::invalid_argument("--target and -o are required");
    const auto target = cgra::TargetModel::loadFromFile(targetPath);
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto rf = cgra::register_allocation::readJson(rfPath);
    const auto schedule = cgra::schedule::readMaterializedSchedule(schedulePath);
    cgra::lowering::TargetLoweringOptions options;
    options.targetPath = targetPath;
    const auto result = cgra::lowering::TargetLowering::lower(dfg, target, rf, schedule, options);
    if (!result.ok()) {
      for (const auto& diagnostic : result.diagnostics)
        std::cerr << diagnostic.message << '\n';
      return 1;
    }
    std::ofstream output(outputPath);
    if (!output)
      throw std::runtime_error("cannot open output manifest");
    output << result.manifest->json << '\n';
    std::cout << "status: success\n"
              << "ii: " << schedule.ii() << "\n"
              << "encoded controls: " << result.stats.encodedControls << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cgra-target-lower: " << error.what() << '\n';
    return 2;
  }
}
