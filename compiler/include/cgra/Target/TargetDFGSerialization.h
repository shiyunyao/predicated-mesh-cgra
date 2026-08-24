// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/TargetDFG.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::target {

std::string dump(const TargetDFG& dfg);
std::string toJson(const TargetDFG& dfg);
TargetDFG parse(std::string_view jsonText);
void writeJson(const TargetDFG& dfg, const std::filesystem::path& path);
TargetDFG readJson(const std::filesystem::path& path);

} // namespace cgra::target
