// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/RegisterAllocation/RFAllocatedMapping.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::register_allocation {

class RFAllocatedMappingSerialization {
public:
  static std::string dump(const RFAllocatedMapping& mapping);
  static std::string toJson(const RFAllocatedMapping& mapping);
  static RFAllocatedMapping parse(std::string_view jsonText);
  static void writeJson(const RFAllocatedMapping& mapping, const std::filesystem::path& path);
  static RFAllocatedMapping readJson(const std::filesystem::path& path);
};

std::string dump(const RFAllocatedMapping& mapping);
std::string toJson(const RFAllocatedMapping& mapping);
RFAllocatedMapping parse(std::string_view jsonText);
void writeJson(const RFAllocatedMapping& mapping, const std::filesystem::path& path);
RFAllocatedMapping readJson(const std::filesystem::path& path);

} // namespace cgra::register_allocation
