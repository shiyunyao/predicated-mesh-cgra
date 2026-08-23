// SPDX-License-Identifier: MIT
#include "TestSeed.h"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace cgra::test {

std::uint64_t getTestSeed(std::optional<std::uint64_t> explicitSeed) {
  if (explicitSeed)
    return *explicitSeed;

  const char* environment = std::getenv("CGRA_TEST_SEED");
  if (environment == nullptr || *environment == '\0')
    return 0;

  const std::string_view value(environment);
  std::uint64_t seed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), seed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    throw std::runtime_error("CGRA_TEST_SEED must be an unsigned 64-bit integer");
  return seed;
}

} // namespace cgra::test
