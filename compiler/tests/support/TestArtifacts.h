// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string_view>
#include <utility>

namespace cgra::test {

class TestArtifacts {
public:
  static TestArtifacts forCase(std::string_view caseName);

  const std::filesystem::path& root() const noexcept { return root_; }
  void writeText(std::string_view name, std::string_view contents) const;
  void copyFile(std::string_view name, const std::filesystem::path& source) const;

private:
  explicit TestArtifacts(std::filesystem::path root) : root_(std::move(root)) {}

  std::filesystem::path root_;
};

} // namespace cgra::test
