// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Schedule/StageScheduler.h"
#include "cgra/Schedule/StagedMappingSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json modulo_mapping.json --target target.json"
               " [-o staged_mapping.json] [--json-report report.json]\n";
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
    const std::string mappingPath = argv[2];
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
      std::cerr << "stage-schedule: target contract error: " << error.what() << '\n';
      return 3;
    }
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto modulo = cgra::mapping::readJson(mappingPath);
    const auto result = cgra::schedule::StageScheduler::schedule(dfg, target, modulo);
    std::cout << result.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream report(reportPath);
      if (!report)
        throw std::runtime_error("cannot write stage scheduling report: " + reportPath);
      report << result.toJson();
    }
    if (result.ok() && !outputPath.empty())
      cgra::schedule::StagedMappingSerialization::writeJson(*result.mapping, outputPath);
    if (result.status == cgra::schedule::StageSchedulingStatus::InvalidTargetDFG ||
        result.status == cgra::schedule::StageSchedulingStatus::InvalidModuloMapping)
      return 1;
    if (result.status == cgra::schedule::StageSchedulingStatus::InternalError)
      return 4;
    return result.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "stage-schedule: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "stage-schedule: internal error\n";
    return 4;
  }
}
