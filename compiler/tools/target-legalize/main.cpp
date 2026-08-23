// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGSerialization.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetDFGVerifier.h"
#include "cgra/Target/TargetLegalizer.h"
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
  std::filesystem::path output;
  std::optional<std::filesystem::path> report;
  bool dumpText = false;
};

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " input.dfg.json --target target.json -o output.target_dfg.json"
               " [--json-report report.json] [--dump-text]\n";
}

std::optional<Options> parseOptions(int argc, char** argv) {
  if (argc < 6)
    return std::nullopt;
  Options options{argv[1], {}, {}, std::nullopt, false};
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--target" && index + 1 < argc) {
      options.target = argv[++index];
    } else if ((argument == "-o" || argument == "--output") && index + 1 < argc) {
      options.output = argv[++index];
    } else if (argument == "--json-report" && index + 1 < argc) {
      options.report = std::filesystem::path(argv[++index]);
    } else if (argument == "--dump-text") {
      options.dumpText = true;
    } else {
      return std::nullopt;
    }
  }
  if (options.target.empty() || options.output.empty())
    return std::nullopt;
  return options;
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parseOptions(argc, argv);
  if (!options) {
    usage(argv[0]);
    return 2;
  }

  try {
    const auto generic = cgra::ir::readJson(options->input);
    const auto target = cgra::TargetModel::loadFromFile(options->target);
    const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
    if (!result.ok()) {
      if (options->report) {
        std::ofstream report(*options->report);
        if (!report)
          throw std::runtime_error("cannot write legalization report: " +
                                   options->report->string());
        report << result.toJson();
      }
      std::cerr << result.format() << '\n';
      return 1;
    }

    const auto targetReport =
        cgra::target::TargetDFGVerifier::verify(*result.dfg, target, &generic);
    if (!targetReport.ok()) {
      if (options->report) {
        std::ofstream report(*options->report);
        if (!report)
          throw std::runtime_error("cannot write target DFG report: " + options->report->string());
        report << targetReport.toJson();
      }
      std::cerr << targetReport.format() << '\n';
      return 3;
    }
    if (options->report) {
      std::ofstream report(*options->report);
      if (!report)
        throw std::runtime_error("cannot write legalization report: " + options->report->string());
      report << result.toJson();
    }
    cgra::target::writeJson(*result.dfg, options->output);
    if (options->dumpText)
      std::cout << cgra::target::dump(*result.dfg);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "target legalization input error: " << error.what() << '\n';
    return 2;
  }
}
