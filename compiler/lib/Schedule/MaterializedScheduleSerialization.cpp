// SPDX-License-Identifier: MIT
#include "cgra/Schedule/MaterializedScheduleSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cgra::schedule {
namespace {

using Json = nlohmann::json;
using cgra::register_allocation::StorageSegmentId;

template <typename T> T required(const Json& object, const char* key) {
  if (!object.contains(key))
    throw std::invalid_argument(std::string("materialized schedule JSON missing ") + key);
  try {
    return object.at(key).get<T>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("materialized schedule field ") + key +
                                " has invalid type: " + error.what());
  }
}

std::string domainName(cgra::mapping::NetworkDomain domain) {
  return domain == cgra::mapping::NetworkDomain::Data ? "data" : "predicate";
}

cgra::mapping::NetworkDomain domainFromName(std::string_view value) {
  if (value == "data")
    return cgra::mapping::NetworkDomain::Data;
  if (value == "predicate")
    return cgra::mapping::NetworkDomain::Predicate;
  throw std::invalid_argument("unknown materialized schedule network domain");
}

std::string directionName(cgra::mapping::Direction direction) {
  switch (direction) {
  case cgra::mapping::Direction::North:
    return "north";
  case cgra::mapping::Direction::South:
    return "south";
  case cgra::mapping::Direction::East:
    return "east";
  case cgra::mapping::Direction::West:
    return "west";
  }
  return "north";
}

cgra::mapping::Direction directionFromName(std::string_view value) {
  if (value == "north")
    return cgra::mapping::Direction::North;
  if (value == "south")
    return cgra::mapping::Direction::South;
  if (value == "east")
    return cgra::mapping::Direction::East;
  if (value == "west")
    return cgra::mapping::Direction::West;
  throw std::invalid_argument("unknown materialized schedule direction");
}

Json tileJson(cgra::mapping::TileCoord tile) { return Json::array({tile.row, tile.col}); }

cgra::mapping::TileCoord parseTile(const Json& value) {
  if (!value.is_array() || value.size() != 2)
    throw std::invalid_argument("materialized schedule tile must be [row, col]");
  return {value.at(0).get<std::uint32_t>(), value.at(1).get<std::uint32_t>()};
}

Json eventJson(const MaterializedEvent& event) {
  Json value = {{"kind", toString(event.kind)}, {"logical_iteration", event.logicalIteration}};
  if (event.node)
    value["node"] = *event.node;
  if (event.edge)
    value["edge"] = *event.edge;
  if (event.segment)
    value["segment"] = *event.segment;
  if (event.liveOut)
    value["live_out"] = *event.liveOut;
  if (event.transportActionIndex)
    value["action"] = *event.transportActionIndex;
  if (event.consumerIterationOffset)
    value["consumer_iteration_offset"] = *event.consumerIterationOffset;
  if (event.boundaryValue) {
    std::visit(
        [&](const auto& source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, cgra::ir::ExternalValueRef>)
            value["boundary_external"] = source.value;
          else
            value["boundary_constant"] = source.value;
        },
        *event.boundaryValue);
  }
  if (event.domain)
    value["domain"] = domainName(*event.domain);
  if (event.tile)
    value["tile"] = tileJson(*event.tile);
  if (event.direction)
    value["direction"] = directionName(*event.direction);
  if (event.physicalRegister) {
    value["register"] = {{"tile", tileJson(event.physicalRegister->tile)},
                         {"bank", event.physicalRegister->bank},
                         {"index", event.physicalRegister->index}};
  }
  return value;
}

MaterializedEvent parseEvent(const Json& value) {
  MaterializedEvent event;
  event.kind = materializedEventKindFromString(required<std::string>(value, "kind"));
  event.logicalIteration = required<std::int64_t>(value, "logical_iteration");
  if (value.contains("node"))
    event.node = value.at("node").get<cgra::target::TargetNodeId>();
  if (value.contains("edge"))
    event.edge = value.at("edge").get<cgra::target::TargetEdgeId>();
  if (value.contains("segment"))
    event.segment = value.at("segment").get<StorageSegmentId>();
  if (value.contains("live_out"))
    event.liveOut = value.at("live_out").get<cgra::ir::LiveOutId>();
  if (value.contains("action"))
    event.transportActionIndex = value.at("action").get<std::uint32_t>();
  if (value.contains("consumer_iteration_offset"))
    event.consumerIterationOffset = value.at("consumer_iteration_offset").get<std::uint32_t>();
  if (value.contains("boundary_external") == value.contains("boundary_constant"))
    if (event.kind == MaterializedEventKind::BoundaryValueInject)
      throw std::invalid_argument("boundary injection must contain one boundary value");
  if (value.contains("boundary_external"))
    event.boundaryValue =
        cgra::ir::ExternalValueRef{value.at("boundary_external").get<cgra::ir::ExternalValueId>()};
  else if (value.contains("boundary_constant"))
    event.boundaryValue =
        cgra::ir::ConstantRef{value.at("boundary_constant").get<cgra::ir::ConstantId>()};
  if (value.contains("domain"))
    event.domain = domainFromName(value.at("domain").get<std::string>());
  if (value.contains("tile"))
    event.tile = parseTile(value.at("tile"));
  if (value.contains("direction"))
    event.direction = directionFromName(value.at("direction").get<std::string>());
  if (value.contains("register")) {
    const auto& reg = value.at("register");
    event.physicalRegister = cgra::register_allocation::PhysicalRegister{
        parseTile(required<Json>(reg, "tile")), required<std::string>(reg, "bank"),
        required<std::uint32_t>(reg, "index")};
  }
  return event;
}

Json phaseJson(const SchedulePhase& phase) {
  Json cycles = Json::array();
  for (const auto& bundle : phase.cycles) {
    Json events = Json::array();
    for (const auto& event : bundle.events)
      events.push_back(eventJson(event));
    cycles.push_back({{"events", std::move(events)}});
  }
  return cycles;
}

SchedulePhase parsePhase(const Json& value) {
  if (!value.is_array())
    throw std::invalid_argument("materialized schedule phase must be an array");
  SchedulePhase phase;
  for (const auto& cycle : value) {
    if (!cycle.is_object() || !cycle.contains("events") || !cycle.at("events").is_array())
      throw std::invalid_argument("materialized schedule cycle must contain events");
    CycleBundle bundle;
    for (const auto& event : cycle.at("events"))
      bundle.events.push_back(parseEvent(event));
    phase.cycles.push_back(std::move(bundle));
  }
  return phase;
}

} // namespace

std::string MaterializedScheduleSerialization::dump(const MaterializedSchedule& schedule) {
  std::ostringstream output;
  output << "MaterializedSchedule II=" << schedule.ii() << " trip_count=" << schedule.tripCount()
         << " prologue=" << schedule.prologue().cycles.size()
         << " kernel_repeats=" << schedule.kernel().repeatCount
         << " epilogue=" << schedule.epilogue().cycles.size() << '\n';
  return output.str();
}

std::string MaterializedScheduleSerialization::toJson(const MaterializedSchedule& schedule) {
  Json root = {{"schema", "cgra.materialized_schedule.debug.v1"},
               {"ii", schedule.ii()},
               {"trip_count", schedule.tripCount()},
               {"time_origin_shift", schedule.timeOriginShift()},
               {"total_logical_cycles", schedule.totalLogicalCycles()},
               {"prologue", phaseJson(schedule.prologue())},
               {"kernel",
                {{"repeat_count", schedule.kernel().repeatCount},
                 {"cycles", phaseJson(SchedulePhase{schedule.kernel().body})}}},
               {"epilogue", phaseJson(schedule.epilogue())}};
  return root.dump(2);
}

MaterializedSchedule MaterializedScheduleSerialization::parse(std::string_view jsonText) {
  const auto root = Json::parse(jsonText);
  if (required<std::string>(root, "schema") != "cgra.materialized_schedule.debug.v1")
    throw std::invalid_argument("unsupported materialized schedule schema");
  const auto ii = required<std::uint32_t>(root, "ii");
  const auto tripCount = required<std::uint64_t>(root, "trip_count");
  const auto shift = required<std::uint64_t>(root, "time_origin_shift");
  const auto total = required<std::uint64_t>(root, "total_logical_cycles");
  const auto prologue = parsePhase(root.at("prologue"));
  const auto epilogue = parsePhase(root.at("epilogue"));
  const auto& kernel = root.at("kernel");
  const auto repeatCount = required<std::uint64_t>(kernel, "repeat_count");
  const auto kernelPhase = parsePhase(kernel.at("cycles"));
  return MaterializedSchedule(ii, tripCount, shift, total, prologue,
                              {kernelPhase.cycles, repeatCount}, epilogue);
}

void MaterializedScheduleSerialization::writeJson(const MaterializedSchedule& schedule,
                                                  const std::filesystem::path& path) {
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("cannot open materialized schedule output: " + path.string());
  output << toJson(schedule) << '\n';
}

MaterializedSchedule
MaterializedScheduleSerialization::readJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open materialized schedule input: " + path.string());
  std::ostringstream content;
  content << input.rdbuf();
  return parse(content.str());
}

std::string dump(const MaterializedSchedule& schedule) {
  return MaterializedScheduleSerialization::dump(schedule);
}
std::string toJson(const MaterializedSchedule& schedule) {
  return MaterializedScheduleSerialization::toJson(schedule);
}
MaterializedSchedule parseMaterializedSchedule(std::string_view jsonText) {
  return MaterializedScheduleSerialization::parse(jsonText);
}
void writeMaterializedSchedule(const MaterializedSchedule& schedule,
                               const std::filesystem::path& path) {
  MaterializedScheduleSerialization::writeJson(schedule, path);
}
MaterializedSchedule readMaterializedSchedule(const std::filesystem::path& path) {
  return MaterializedScheduleSerialization::readJson(path);
}

} // namespace cgra::schedule
