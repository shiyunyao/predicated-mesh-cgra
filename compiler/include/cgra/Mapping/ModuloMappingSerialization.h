// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Mapping/ModuloMapping.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::mapping {

std::string dump(const ModuloMapping& mapping);
std::string toJson(const ModuloMapping& mapping);
ModuloMapping parse(std::string_view jsonText);
void writeJson(const ModuloMapping& mapping, const std::filesystem::path& path);
ModuloMapping readJson(const std::filesystem::path& path);

} // namespace cgra::mapping
