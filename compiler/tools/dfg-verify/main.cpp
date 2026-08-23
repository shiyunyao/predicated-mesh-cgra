// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGSerialization.h"
#include "cgra/IR/DFGVerifier.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program << " input.dfg.json [--json-report report.json]\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 4) {
    usage(argv[0]);
    return 2;
  }
  if (argc == 4 && std::string(argv[2]) != "--json-report") {
    usage(argv[0]);
    return 2;
  }

  try {
    const auto graph = cgra::ir::readJson(argv[1]);
    const auto report = cgra::ir::DFGVerifier::verify(graph);
    if (argc == 4)
      report.writeJson(argv[3]);
    std::cout << report.format() << '\n';
    return report.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "DFG verification input error: " << error.what() << '\n';
    return 2;
  }
}
