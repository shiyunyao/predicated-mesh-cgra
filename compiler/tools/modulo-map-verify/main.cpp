// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json mapping.json --target target.json [--json-report report.json]\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    usage(argv[0]);
    return 2;
  }
  std::string dfgPath = argv[1];
  std::string mappingPath = argv[2];
  std::string targetPath;
  std::string reportPath;
  for (int index = 3; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--target" && index + 1 < argc)
      targetPath = argv[++index];
    else if (option == "--json-report" && index + 1 < argc)
      reportPath = argv[++index];
    else {
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
    std::cerr << "modulo-map-verify: target contract error: " << error.what() << '\n';
    return 3;
  }
  try {
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto mapping = cgra::mapping::readJson(mappingPath);
    const auto report = cgra::mapping::ModuloMappingVerifier::verify(dfg, target, mapping);
    std::cout << report.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream output(reportPath);
      if (!output)
        throw std::runtime_error("cannot write verification report: " + reportPath);
      output << report.toJson();
    }
    return report.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "modulo-map-verify: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "modulo-map-verify: internal error\n";
    return 4;
  }
}
