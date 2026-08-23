// SPDX-License-Identifier: MIT
#include "support/Metrics.h"
#include "support/TestArtifacts.h"
#include "support/TestSeed.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(CompilerTestSupport, ResolvesExplicitAndEnvironmentIndependentSeed) {
  EXPECT_EQ(cgra::test::getTestSeed(42), 42U);
}

TEST(CompilerTestSupport, WritesAndCopiesArtifacts) {
  const auto artifacts = cgra::test::TestArtifacts::forCase("gtest_smoke");
  artifacts.writeText("notes.txt", "gtest smoke");
  artifacts.copyFile("target.json",
                     std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  EXPECT_TRUE(std::filesystem::is_regular_file(artifacts.root() / "notes.txt"));
  EXPECT_TRUE(std::filesystem::is_regular_file(artifacts.root() / "target.json"));
}

TEST(CompilerTestSupport, ValidatesMetricsFixture) {
  std::string error;
  EXPECT_TRUE(cgra::test::validateMetrics(cgra::test::metricsFixture(), &error)) << error;
}
