// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/IR/DFG.h"

#include <string_view>
#include <vector>

namespace cgra::ir::fixtures {

DFG simpleAdd();
DFG arithmeticChain();
DFG fanout();
DFG recurrence();
DFG loadAddStore();
DFG predicateSelect();
DFG predicateSelectUnsigned();
DFG predicatedStore();
DFG memoryDependence();
std::vector<DFG> all();

} // namespace cgra::ir::fixtures
