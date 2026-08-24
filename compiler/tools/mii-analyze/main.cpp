// SPDX-License-Identifier: MIT
#include "cgra/Analysis/MIIAnalyzer.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct Options {
  std::filesystem::path input;
  std::filesystem::path target;
  std::optional<std::filesystem::path> report;
};

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json --target target.json [--json-report report.json]\n";
}

std::optional<Options> parseOptions(int argc, char** argv) {
  if (argc < 4)
    return std::nullopt;
  Options options{argv[1], {}, std::nullopt};
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--target" && index + 1 < argc) {
      options.target = argv[++index];
    } else if (argument == "--json-report" && index + 1 < argc) {
      options.report = std::filesystem::path(argv[++index]);
    } else {
      return std::nullopt;
    }
  }
  if (options.target.empty())
    return std::nullopt;
  return options;
}

int exitCode(cgra::analysis::MIIStatus status) {
  using cgra::analysis::MIIStatus;
  switch (status) {
  case MIIStatus::Success:
    return 0;
  case MIIStatus::InvalidTargetDFG:
  case MIIStatus::NoCompatibleResource:
  case MIIStatus::UnschedulableZeroDistanceCycle:
    return 1;
  case MIIStatus::TargetContractError:
    return 3;
  case MIIStatus::InternalError:
    return 4;
  }
  return 4;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parseOptions(argc, argv);
  if (!options) {
    usage(argv[0]);
    return 2;
  }

  cgra::TargetModel target;
  try {
    target = cgra::TargetModel::loadFromFile(options->target);
  } catch (const std::exception& error) {
    std::cerr << "mii-analyze: target contract error: " << error.what() << '\n';
    return 3;
  }

  try {
    const auto dfg = cgra::target::readJson(options->input);
    const auto result = cgra::analysis::MIIAnalyzer::analyze(dfg, target);
    if (options->report) {
      std::ofstream output(*options->report);
      if (!output)
        throw std::runtime_error("cannot write MII report: " + options->report->string());
      output << result.toJson();
    }
    std::cout << result.format() << '\n';
    return exitCode(result.status);
  } catch (const std::exception& error) {
    std::cerr << "mii-analyze: input error: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "mii-analyze: internal error\n";
    return 4;
  }
}
