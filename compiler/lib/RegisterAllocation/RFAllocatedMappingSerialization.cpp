// SPDX-License-Identifier: MIT
#include "cgra/RegisterAllocation/RFAllocatedMappingSerialization.h"

#include "cgra/Schedule/StagedMappingSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cgra::register_allocation {
namespace {
using Json = nlohmann::json;

template <typename T> T required(const Json& object, const char* key) {
  if (!object.contains(key))
    throw std::invalid_argument(std::string("RF mapping JSON missing ") + key);
  try {
    return object.at(key).get<T>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("RF mapping JSON field ") + key +
                                " has invalid type: " + error.what());
  }
}

Json tileJson(cgra::mapping::TileCoord tile) { return Json::array({tile.row, tile.col}); }

cgra::mapping::TileCoord parseTile(const Json& value) {
  if (!value.is_array() || value.size() != 2)
    throw std::invalid_argument("RF mapping tile must be [row, col]");
  return {value.at(0).get<std::uint32_t>(), value.at(1).get<std::uint32_t>()};
}

std::string domainName(cgra::RegisterBankDomain domain) {
  return domain == cgra::RegisterBankDomain::Data ? "data" : "predicate";
}

cgra::RegisterBankDomain domainFromName(std::string_view name) {
  if (name == "data")
    return cgra::RegisterBankDomain::Data;
  if (name == "predicate")
    return cgra::RegisterBankDomain::Predicate;
  throw std::invalid_argument("unknown RF bank domain");
}
} // namespace

std::string RFAllocatedMappingSerialization::dump(const RFAllocatedMapping& mapping) {
  std::ostringstream output;
  output << "RFAllocatedMapping II=" << mapping.staged().modulo().ii()
         << " segments=" << mapping.storageRequirements().segments().size() << '\n';
  for (const auto& allocation : mapping.allocations())
    output << "  segment=" << allocation.segment << " tile=" << allocation.reg.tile.row << ','
           << allocation.reg.tile.col << " bank=" << allocation.reg.bank
           << " register=" << allocation.reg.index << '\n';
  return output.str();
}

std::string RFAllocatedMappingSerialization::toJson(const RFAllocatedMapping& mapping) {
  Json root = {{"schema", "cgra.rf_allocated_mapping.debug.v1"},
               {"ii", mapping.staged().modulo().ii()},
               {"staged_mapping", Json::parse(cgra::schedule::toJson(mapping.staged()))},
               {"storage_segments", Json::array()}};
  for (const auto& segment : mapping.storageRequirements().segments()) {
    const auto& allocation = mapping.allocationFor(segment.id);
    Json segmentJson = {{"id", segment.id},
                        {"edge", segment.edge},
                        {"tile", tileJson(segment.tile)},
                        {"bank", allocation.reg.bank},
                        {"domain", domainName(segment.domain)},
                        {"write_time", segment.writeTime},
                        {"read_time", segment.readTime},
                        {"origins", Json::array()},
                        {"register", allocation.reg.index},
                        {"read_port", allocation.readPort},
                        {"write_port", allocation.writePort}};
    segmentJson["phase_count"] = allocation.family.phaseCount;
    segmentJson["phases"] = Json::array();
    for (const auto& phase : allocation.family.phases)
      segmentJson["phases"].push_back({{"phase", phase.phase}, {"register", phase.reg.index}});
    if (allocation.boundaryWritePort)
      segmentJson["boundary_write_port"] = *allocation.boundaryWritePort;
    segmentJson["boundary_writes"] = Json::array();
    for (const auto& boundary : allocation.boundaryWrites)
      segmentJson["boundary_writes"].push_back(
          {{"producer_iteration", boundary.producerIteration}, {"write_port", boundary.writePort}});
    root["storage_segments"].push_back(std::move(segmentJson));
    for (const auto& origin : segment.origins) {
      Json originJson = {{"kind", origin.kind == StorageOriginKind::ExplicitVirtualHold
                                      ? "virtual_hold"
                                      : "terminal_slack"},
                         {"edge", origin.edge}};
      if (origin.transportActionIndex)
        originJson["action"] = *origin.transportActionIndex;
      root["storage_segments"].back()["origins"].push_back(std::move(originJson));
    }
  }
  return root.dump(2);
}

RFAllocatedMapping RFAllocatedMappingSerialization::parse(std::string_view jsonText) {
  const auto root = Json::parse(jsonText);
  if (required<std::string>(root, "schema") != "cgra.rf_allocated_mapping.debug.v1")
    throw std::invalid_argument("unsupported RF allocated mapping schema");
  const auto staged = cgra::schedule::parse(required<Json>(root, "staged_mapping").dump());
  const auto& segments = root.at("storage_segments");
  if (!segments.is_array())
    throw std::invalid_argument("RF mapping storage_segments must be an array");
  std::vector<StorageSegment> requirementsSegments;
  std::vector<StorageAllocation> allocations;
  for (const auto& entry : segments) {
    const auto id = required<StorageSegmentId>(entry, "id");
    const auto tile = parseTile(entry.at("tile"));
    const auto domain = domainFromName(required<std::string>(entry, "domain"));
    std::vector<StorageOrigin> origins;
    if (entry.contains("origins")) {
      for (const auto& origin : entry.at("origins")) {
        const auto kind = required<std::string>(origin, "kind");
        StorageOriginKind originKind;
        if (kind == "virtual_hold")
          originKind = StorageOriginKind::ExplicitVirtualHold;
        else if (kind == "terminal_slack")
          originKind = StorageOriginKind::TerminalSlack;
        else
          throw std::invalid_argument("unknown RF storage origin kind: " + kind);
        origins.push_back({originKind, required<cgra::target::TargetEdgeId>(origin, "edge"),
                           origin.contains("action") ? std::optional<std::uint32_t>(
                                                           origin.at("action").get<std::uint32_t>())
                                                     : std::nullopt});
      }
    }
    requirementsSegments.push_back({id, required<cgra::target::TargetEdgeId>(entry, "edge"), tile,
                                    domain, required<std::uint64_t>(entry, "write_time"),
                                    required<std::uint64_t>(entry, "read_time"),
                                    std::move(origins)});
    const auto bankName = required<std::string>(entry, "bank");
    const auto registerIndex = required<std::uint32_t>(entry, "register");
    PeriodicRegisterFamily family;
    family.segment = id;
    family.phaseCount = entry.contains("phase_count")
                            ? entry.at("phase_count").get<std::uint32_t>()
                            : 1U;
    if (entry.contains("phases")) {
      for (const auto& phase : entry.at("phases"))
        family.phases.push_back(
            {required<std::uint32_t>(phase, "phase"),
             {tile, bankName, required<std::uint32_t>(phase, "register")}});
    }
    if (family.phases.empty())
      family.phases.push_back({0, {tile, bankName, registerIndex}});
    std::vector<BoundaryWriteAllocation> boundaryWrites;
    if (entry.contains("boundary_writes"))
      for (const auto& boundary : entry.at("boundary_writes"))
        boundaryWrites.push_back({required<std::int64_t>(boundary, "producer_iteration"),
                                  required<std::uint32_t>(boundary, "write_port")});
    if (boundaryWrites.empty() && entry.contains("boundary_write_port") &&
        !entry.at("boundary_write_port").is_null())
      boundaryWrites.push_back({-1, entry.at("boundary_write_port").get<std::uint32_t>()});
    allocations.push_back({id,
                            {tile, bankName, registerIndex},
                            entry.contains("read_port") ? entry.at("read_port").get<std::uint32_t>()
                                                         : 0U,
                            entry.contains("write_port")
                                ? entry.at("write_port").get<std::uint32_t>()
                                : 0U,
                            entry.contains("boundary_write_port") &&
                                    !entry.at("boundary_write_port").is_null()
                                ? std::optional<std::uint32_t>(
                                      entry.at("boundary_write_port").get<std::uint32_t>())
                                : std::nullopt,
                            std::move(family),
                            std::move(boundaryWrites)});
  }
  StorageRequirements requirements(staged.modulo().ii(), std::move(requirementsSegments));
  return RFAllocatedMapping(staged, std::move(requirements), std::move(allocations));
}

void RFAllocatedMappingSerialization::writeJson(const RFAllocatedMapping& mapping,
                                                const std::filesystem::path& path) {
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("cannot open RF mapping output: " + path.string());
  output << toJson(mapping) << '\n';
}

RFAllocatedMapping RFAllocatedMappingSerialization::readJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open RF mapping input: " + path.string());
  std::ostringstream content;
  content << input.rdbuf();
  return parse(content.str());
}

std::string dump(const RFAllocatedMapping& mapping) {
  return RFAllocatedMappingSerialization::dump(mapping);
}
std::string toJson(const RFAllocatedMapping& mapping) {
  return RFAllocatedMappingSerialization::toJson(mapping);
}
RFAllocatedMapping parse(std::string_view jsonText) {
  return RFAllocatedMappingSerialization::parse(jsonText);
}
void writeJson(const RFAllocatedMapping& mapping, const std::filesystem::path& path) {
  RFAllocatedMappingSerialization::writeJson(mapping, path);
}
RFAllocatedMapping readJson(const std::filesystem::path& path) {
  return RFAllocatedMappingSerialization::readJson(path);
}

} // namespace cgra::register_allocation
