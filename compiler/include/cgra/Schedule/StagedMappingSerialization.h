// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/StagedMapping.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::schedule {

class StagedMappingSerialization {
public:
  static std::string dump(const StagedMapping& mapping);
  static std::string toJson(const StagedMapping& mapping);
  static StagedMapping parse(std::string_view jsonText);
  static void writeJson(const StagedMapping& mapping, const std::filesystem::path& path);
  static StagedMapping readJson(const std::filesystem::path& path);
};

std::string dump(const StagedMapping& mapping);
std::string toJson(const StagedMapping& mapping);
StagedMapping parse(std::string_view jsonText);
void writeJson(const StagedMapping& mapping, const std::filesystem::path& path);
StagedMapping readJson(const std::filesystem::path& path);

} // namespace cgra::schedule
