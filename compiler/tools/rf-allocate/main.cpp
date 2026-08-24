// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/RegisterAllocation/RFAllocator.h"
#include "cgra/Schedule/StagedMappingSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {
void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json staged_mapping.json --target target.json"
               " [-o rf_mapping.json] [--json-report report.json]\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    usage(argv[0]);
    return 2;
  }
  std::string targetPath;
  std::string outputPath;
  std::string reportPath;
  try {
    const std::string dfgPath = argv[1];
    const std::string stagedPath = argv[2];
    for (int index = 3; index < argc; ++index) {
      const std::string option = argv[index];
      if ((option == "--target" || option == "-o" || option == "--json-report") &&
          index + 1 < argc) {
        auto& destination = option == "--target" ? targetPath
                            : option == "-o"     ? outputPath
                                                 : reportPath;
        destination = argv[++index];
      } else {
        usage(argv[0]);
        return 2;
      }
    }
    if (targetPath.empty()) {
      usage(argv[0]);
      return 2;
    }
    cgra::TargetModel target;
    try {
      target = cgra::TargetModel::loadFromFile(targetPath);
    } catch (const std::exception& error) {
      std::cerr << "rf-allocate: target contract error: " << error.what() << '\n';
      return 3;
    }
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto staged = cgra::schedule::readJson(stagedPath);
    const auto result = cgra::register_allocation::RFAllocator::allocate(dfg, target, staged);
    std::cout << result.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream report(reportPath);
      if (!report)
        throw std::runtime_error("cannot write RF allocation report: " + reportPath);
      report << result.toJson();
    }
    if (result.ok() && !outputPath.empty())
      cgra::register_allocation::writeJson(*result.mapping, outputPath);
    return result.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "rf-allocate: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "rf-allocate: internal error\n";
    return 4;
  }
}
