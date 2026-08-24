// SPDX-License-Identifier: MIT
#include "support/Metrics.h"
#include "support/TestArtifacts.h"
#include "support/TestSeed.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
  try {
    std::uint64_t expectedSeed = 0;
    if (argc == 3 && std::string_view(argv[1]) == "--expect-seed")
      expectedSeed = std::stoull(argv[2]);
    else if (argc != 1)
      throw std::runtime_error("usage: cgra-test-support [--expect-seed N]");

    if (cgra::test::getTestSeed() != expectedSeed || cgra::test::getTestSeed(42) != 42)
      throw std::runtime_error("test seed resolution failed");

    const auto artifacts = cgra::test::TestArtifacts::forCase("support_smoke");
    artifacts.writeText("notes.txt", "support smoke");
    artifacts.copyFile("target.json",
                       std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
    std::ifstream notes(artifacts.root() / "notes.txt");
    std::ifstream target(artifacts.root() / "target.json");
    if (!notes || !target)
      throw std::runtime_error("artifact files were not created");

    const auto metrics = cgra::test::metricsFixture();
    artifacts.writeText("metrics.json", metrics.dump(2));
    std::string error;
    if (!cgra::test::validateMetrics(metrics, &error))
      throw std::runtime_error("metrics fixture is invalid: " + error);
    std::cout << "CGRA_TEST_SUPPORT_PASS seed=" << cgra::test::getTestSeed() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "CGRA_TEST_SUPPORT_FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
