// SPDX-License-Identifier: MIT
#include "TestArtifacts.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cgra::test {
namespace {

std::filesystem::path artifactBase() {
  if (const char* environment = std::getenv("CGRA_TEST_ARTIFACT_DIR");
      environment != nullptr && *environment != '\0')
    return environment;
  return std::filesystem::current_path() / "build" / "failures";
}

std::filesystem::path safeChild(const std::filesystem::path& root, std::string_view name) {
  const std::filesystem::path child(name);
  if (child.empty() || child.is_absolute() || child.has_parent_path())
    throw std::invalid_argument("test artifact name must be a relative file name");
  return root / child;
}

} // namespace

TestArtifacts TestArtifacts::forCase(std::string_view caseName) {
  const std::filesystem::path name(caseName);
  if (name.empty() || name.is_absolute() || name.has_parent_path())
    throw std::invalid_argument("test case name must be a single relative path component");

  auto root = artifactBase() / name;
  std::filesystem::create_directories(root);
  return TestArtifacts(std::move(root));
}

void TestArtifacts::writeText(std::string_view name, std::string_view contents) const {
  const auto path = safeChild(root_, name);
  std::ofstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("cannot write test artifact: " + path.string());
  stream << contents;
}

void TestArtifacts::copyFile(std::string_view name, const std::filesystem::path& source) const {
  const auto path = safeChild(root_, name);
  if (!std::filesystem::is_regular_file(source))
    throw std::runtime_error("test artifact source is not a regular file: " + source.string());
  if (!std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing))
    throw std::runtime_error("cannot copy test artifact: " + path.string());
}

} // namespace cgra::test
