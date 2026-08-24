// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cgra::ir {

std::string dump(const DFG& dfg);
std::string toJson(const DFG& dfg);
DFG parse(std::string_view jsonText);
void writeJson(const DFG& dfg, const std::filesystem::path& path);
DFG readJson(const std::filesystem::path& path);

} // namespace cgra::ir
