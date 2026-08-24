// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"
#include "cgra/Schedule/MaterializedScheduleSerialization.h"
#include "cgra/Schedule/ScheduleMaterializer.h"
#include "cgra/Target/TargetDFGSerialization.h"
#include "cgra/Target/TargetModel.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " target_dfg.json rf_allocated_mapping.json --target target.json"
               " --trip-count N [-o materialized_schedule.json]"
               " [--json-report report.json]\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    usage(argv[0]);
    return 2;
  }
  std::string targetPath;
  std::string outputPath;
  std::string reportPath;
  std::uint64_t tripCount = 0;
  try {
    const std::string dfgPath = argv[1];
    const std::string mappingPath = argv[2];
    for (int index = 3; index < argc; ++index) {
      const std::string option = argv[index];
      if ((option == "--target" || option == "--trip-count" || option == "-o" ||
           option == "--json-report") &&
          index + 1 < argc) {
        const auto value = std::string(argv[++index]);
        if (option == "--target")
          targetPath = value;
        else if (option == "--trip-count")
          tripCount = std::stoull(value);
        else if (option == "-o")
          outputPath = value;
        else
          reportPath = value;
      } else {
        usage(argv[0]);
        return 2;
      }
    }
    if (targetPath.empty() || tripCount == 0) {
      usage(argv[0]);
      return 2;
    }
    const auto target = cgra::TargetModel::loadFromFile(targetPath);
    const auto dfg = cgra::target::readJson(dfgPath);
    const auto mapping =
        cgra::register_allocation::RFAllocatedMappingSerialization::readJson(mappingPath);
    cgra::schedule::ScheduleMaterializationRequest request;
    request.tripCount = tripCount;
    const auto result =
        cgra::schedule::ScheduleMaterializer::materialize(dfg, target, mapping, request);
    std::cout << result.format() << '\n';
    if (!reportPath.empty()) {
      std::ofstream report(reportPath);
      if (!report)
        throw std::runtime_error("cannot write materialization report: " + reportPath);
      report << result.toJson();
    }
    if (result.ok() && !outputPath.empty())
      cgra::schedule::MaterializedScheduleSerialization::writeJson(*result.schedule, outputPath);
    if (result.status == cgra::schedule::ScheduleMaterializationStatus::InternalError)
      return 4;
    return result.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "materialize-schedule: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "materialize-schedule: internal error\n";
    return 4;
  }
}
