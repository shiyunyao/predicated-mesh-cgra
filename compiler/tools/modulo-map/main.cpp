// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMapper.h"
#include "cgra/Mapping/ModuloMappingSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json --target target.json --max-ii II"
               " [-o mapping.json] [--json-report report.json]"
               " [--min-ii N] [--max-node-attempts N] [--max-backtracks N]"
               " [--max-route-calls N] [--route-max-expansions N]\n";
}

std::uint64_t parseUnsigned(const std::string& value, const char* option) {
  std::size_t consumed = 0;
  const auto parsed = std::stoull(value, &consumed);
  if (consumed != value.size())
    throw std::invalid_argument(std::string("invalid value for ") + option);
  return parsed;
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
  cgra::mapping::ModuloMapperOptions options;
  try {
    const std::string dfgPath = argv[1];
    for (int index = 2; index < argc; ++index) {
      const std::string option = argv[index];
      auto next = [&]() {
        if (index + 1 >= argc)
          throw std::invalid_argument("missing value for " + option);
        return std::string(argv[++index]);
      };
      if (option == "--target")
        targetPath = next();
      else if (option == "--max-ii")
        options.maxII = static_cast<std::uint32_t>(parseUnsigned(next(), "--max-ii"));
      else if (option == "--min-ii")
        options.minII = static_cast<std::uint32_t>(parseUnsigned(next(), "--min-ii"));
      else if (option == "--max-node-attempts")
        options.budget.maxNodeCandidateAttempts = parseUnsigned(next(), "--max-node-attempts");
      else if (option == "--max-backtracks")
        options.budget.maxBacktracks = parseUnsigned(next(), "--max-backtracks");
      else if (option == "--max-route-calls")
        options.budget.maxRouteSearchCalls = parseUnsigned(next(), "--max-route-calls");
      else if (option == "--route-max-expansions")
        options.budget.perRouteBudget.maxStateExpansions =
            parseUnsigned(next(), "--route-max-expansions");
      else if (option == "-o")
        outputPath = next();
      else if (option == "--json-report")
        reportPath = next();
      else {
        usage(argv[0]);
        return 2;
      }
    }
    if (targetPath.empty() || options.maxII == 0) {
      usage(argv[0]);
      return 2;
    }
    const auto target = cgra::TargetModel::loadFromFile(targetPath);
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto result = cgra::mapping::ModuloMapper::map(dfg, target, options);
    std::cout << result.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream report(reportPath);
      if (!report)
        throw std::runtime_error("cannot write mapper report: " + reportPath);
      report << result.toJson();
    }
    if (result.ok() && !outputPath.empty())
      cgra::mapping::writeJson(*result.mapping, outputPath);
    if (!result.ok() && result.status == cgra::mapping::ModuloMapperStatus::TargetContractError)
      return 3;
    return result.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "modulo-map: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "modulo-map: internal error\n";
    return 4;
  }
}
