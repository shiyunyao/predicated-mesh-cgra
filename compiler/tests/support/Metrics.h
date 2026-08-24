// SPDX-License-Identifier: MIT
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace cgra::test {

inline nlohmann::json metricsFixture() {
  return {
      {"schema", "cgra.compiler.metrics.v1"},
      {"case", "placeholder"},
      {"component", "target_model"},
      {"status", "success"},
      {"seed", std::uint64_t{0}},
      {"wall_time_ms", 1.23},
      {"metrics", nlohmann::json::object()},
  };
}

inline bool validateMetrics(const nlohmann::json& value, std::string* error = nullptr) {
  const auto fail = [&](std::string message) {
    if (error != nullptr)
      *error = std::move(message);
    return false;
  };
  if (!value.is_object())
    return fail("metrics record must be an object");
  if (value.value("schema", "") != "cgra.compiler.metrics.v1")
    return fail("unsupported metrics schema");
  for (const char* key : {"case", "component", "status"}) {
    if (!value.contains(key) || !value.at(key).is_string() || value.at(key).empty())
      return fail(std::string("metrics field must be a non-empty string: ") + key);
  }
  const auto status = value.at("status").get<std::string>();
  if (status != "success" && status != "infeasible" && status != "budget_exceeded" &&
      status != "invalid_input" && status != "internal_error")
    return fail("unsupported metrics status");
  if (!value.contains("seed") || !value.at("seed").is_number_unsigned())
    return fail("metrics.seed must be unsigned");
  if (!value.contains("wall_time_ms") || !value.at("wall_time_ms").is_number())
    return fail("metrics.wall_time_ms must be numeric");
  if (!value.contains("metrics") || !value.at("metrics").is_object())
    return fail("metrics.metrics must be an object");
  return true;
}

} // namespace cgra::test
