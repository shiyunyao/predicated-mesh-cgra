// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>

namespace cgra::test {

// Resolve an explicit seed first, then CGRA_TEST_SEED, then the documented zero default.
std::uint64_t getTestSeed(std::optional<std::uint64_t> explicitSeed = std::nullopt);

} // namespace cgra::test
