// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloRouteSearch.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json --target target.json --ii II --edge ID"
               " --producer-node ID --producer-tile ROW,COL --producer-slot SLOT"
               " --consumer-node ID --consumer-tile ROW,COL --consumer-slot SLOT"
               " [--max-state-expansions N] [--max-queue-pushes N]"
               " [--no-hold] [--json-report report.json]\n";
}

std::uint32_t parseUnsigned(const std::string& text, const char* option) {
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed);
  if (consumed != text.size() || value > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument(std::string("invalid ") + option + " value");
  return static_cast<std::uint32_t>(value);
}

cgra::mapping::TileCoord parseTile(const std::string& text, const char* option) {
  const auto comma = text.find(',');
  if (comma == std::string::npos)
    throw std::invalid_argument(std::string("invalid ") + option + " tile");
  return {parseUnsigned(text.substr(0, comma), option),
          parseUnsigned(text.substr(comma + 1), option)};
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 13) {
    usage(argv[0]);
    return 2;
  }
  std::string targetPath;
  std::string reportPath;
  std::string dfgPath = argv[1];
  cgra::mapping::RouteSearchRequest request;
  cgra::mapping::RouteSearchOptions options;
  std::uint32_t ii = 0;
  try {
    for (int index = 2; index < argc; ++index) {
      const std::string option = argv[index];
      auto next = [&]() -> std::string {
        if (index + 1 >= argc)
          throw std::invalid_argument("missing value for " + option);
        return argv[++index];
      };
      if (option == "--target")
        targetPath = next();
      else if (option == "--ii")
        ii = parseUnsigned(next(), "--ii");
      else if (option == "--edge")
        request.edge = parseUnsigned(next(), "--edge");
      else if (option == "--producer-node")
        request.producer.node = parseUnsigned(next(), "--producer-node");
      else if (option == "--producer-tile")
        request.producer.tile = parseTile(next(), "--producer-tile");
      else if (option == "--producer-slot")
        request.producer.issueSlot =
            cgra::mapping::ModuloSlot(parseUnsigned(next(), "--producer-slot"));
      else if (option == "--consumer-node")
        request.consumer.node = parseUnsigned(next(), "--consumer-node");
      else if (option == "--consumer-tile")
        request.consumer.tile = parseTile(next(), "--consumer-tile");
      else if (option == "--consumer-slot")
        request.consumer.issueSlot =
            cgra::mapping::ModuloSlot(parseUnsigned(next(), "--consumer-slot"));
      else if (option == "--max-state-expansions")
        options.budget.maxStateExpansions = std::stoull(next());
      else if (option == "--max-queue-pushes")
        options.budget.maxQueuePushes = std::stoull(next());
      else if (option == "--no-hold")
        options.allowVirtualHold = false;
      else if (option == "--json-report")
        reportPath = next();
      else {
        usage(argv[0]);
        return 2;
      }
    }
    if (targetPath.empty() || ii == 0) {
      usage(argv[0]);
      return 2;
    }
    const auto target = cgra::TargetModel::loadFromFile(targetPath);
    const auto dfg = cgra::target::readJson(dfgPath);
    cgra::mapping::ModuloResourceModel resources(target, ii);
    cgra::mapping::ResourceReservationTable reservations(resources);
    const auto result = cgra::mapping::ModuloRouteSearch::search(dfg, target, resources,
                                                                 reservations, request, options);
    std::cout << result.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream output(reportPath);
      if (!output)
        throw std::runtime_error("cannot write route report: " + reportPath);
      output << result.toJson(request.edge);
    }
    return result.ok()
               ? 0
               : (result.status == cgra::mapping::RouteSearchStatus::TargetContractError ? 3 : 1);
  } catch (const std::exception& error) {
    std::cerr << "modulo-route-search: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "modulo-route-search: internal error\n";
    return 4;
  }
}
