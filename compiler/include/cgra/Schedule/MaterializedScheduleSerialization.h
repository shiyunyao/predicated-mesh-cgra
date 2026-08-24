// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Schedule/MaterializedSchedule.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::schedule {

class MaterializedScheduleSerialization {
public:
  static std::string dump(const MaterializedSchedule& schedule);
  static std::string toJson(const MaterializedSchedule& schedule);
  static MaterializedSchedule parse(std::string_view jsonText);
  static void writeJson(const MaterializedSchedule& schedule, const std::filesystem::path& path);
  static MaterializedSchedule readJson(const std::filesystem::path& path);
};

std::string dump(const MaterializedSchedule& schedule);
std::string toJson(const MaterializedSchedule& schedule);
MaterializedSchedule parseMaterializedSchedule(std::string_view jsonText);
void writeMaterializedSchedule(const MaterializedSchedule& schedule,
                               const std::filesystem::path& path);
MaterializedSchedule readMaterializedSchedule(const std::filesystem::path& path);

} // namespace cgra::schedule
