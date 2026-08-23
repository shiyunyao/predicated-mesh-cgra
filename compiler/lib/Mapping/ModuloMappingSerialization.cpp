// SPDX-License-Identifier: MIT
#include "cgra/Mapping/ModuloMappingSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace cgra::mapping {
namespace {

using Json = nlohmann::json;

std::string directionName(Direction direction) {
  switch (direction) {
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::East:
    return "east";
  case Direction::West:
    return "west";
  }
  return "north";
}

Direction directionFromName(std::string_view name) {
  if (name == "north")
    return Direction::North;
  if (name == "south")
    return Direction::South;
  if (name == "east")
    return Direction::East;
  if (name == "west")
    return Direction::West;
  throw std::invalid_argument("unknown mapping direction: " + std::string(name));
}

std::string domainName(NetworkDomain domain) {
  return domain == NetworkDomain::Data ? "data" : "predicate";
}

NetworkDomain domainFromName(std::string_view name) {
  if (name == "data")
    return NetworkDomain::Data;
  if (name == "predicate")
    return NetworkDomain::Predicate;
  throw std::invalid_argument("unknown mapping network domain: " + std::string(name));
}

std::string kindName(cgra::ir::Edge::Kind kind) {
  switch (kind) {
  case cgra::ir::Edge::Kind::Data:
    return "data";
  case cgra::ir::Edge::Kind::Predicate:
    return "predicate";
  case cgra::ir::Edge::Kind::Memory:
    return "memory";
  }
  return "data";
}

cgra::ir::Edge::Kind kindFromName(std::string_view name) {
  if (name == "data")
    return cgra::ir::Edge::Kind::Data;
  if (name == "predicate")
    return cgra::ir::Edge::Kind::Predicate;
  if (name == "memory")
    return cgra::ir::Edge::Kind::Memory;
  throw std::invalid_argument("unknown mapping dependence kind: " + std::string(name));
}

template <typename T> T required(const Json& object, std::string_view key) {
  if (!object.contains(std::string(key)))
    throw std::invalid_argument("mapping JSON missing " + std::string(key));
  try {
    return object.at(std::string(key)).get<T>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument("mapping JSON field " + std::string(key) +
                                " has invalid type: " + error.what());
  }
}

Json tileJson(TileCoord tile) { return Json::array({tile.row, tile.col}); }

TileCoord parseTile(const Json& value) {
  if (!value.is_array() || value.size() != 2)
    throw std::invalid_argument("mapping tile must be [row, col]");
  return {value.at(0).get<std::uint32_t>(), value.at(1).get<std::uint32_t>()};
}

Json actionJson(const TransportAction& action) {
  return std::visit(
      [](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LinkStep>) {
          return {{"kind", "link"},
                  {"domain", domainName(value.domain)},
                  {"source", tileJson(value.source)},
                  {"direction", directionName(value.direction)},
                  {"elapsed", value.elapsedFromProducerIssue}};
        } else {
          return {{"kind", "hold"},
                  {"domain", domainName(value.domain)},
                  {"tile", tileJson(value.tile)},
                  {"capture_elapsed", value.captureElapsed},
                  {"release_elapsed", value.releaseElapsed}};
        }
      },
      action);
}

TransportAction parseAction(const Json& value) {
  const auto kind = required<std::string>(value, "kind");
  const auto domain = domainFromName(required<std::string>(value, "domain"));
  if (kind == "link")
    return LinkStep{domain, parseTile(value.at("source")),
                    directionFromName(required<std::string>(value, "direction")),
                    required<std::uint32_t>(value, "elapsed")};
  if (kind == "hold")
    return VirtualHold{domain, parseTile(value.at("tile")),
                       required<std::uint32_t>(value, "capture_elapsed"),
                       required<std::uint32_t>(value, "release_elapsed")};
  throw std::invalid_argument("unknown mapping transport action kind: " + kind);
}

} // namespace

std::string dump(const ModuloMapping& mapping) {
  std::ostringstream output;
  output << "ModuloMapping target=\"" << mapping.targetName() << "\" II=" << mapping.ii() << "\n\n";
  output << "placements:\n";
  for (const auto& placement : mapping.placements())
    output << "  %n" << placement.node << " -> " << toString(placement.tile)
           << " slot=" << placement.issueSlot.value() << '\n';
  output << "\ndependences:\n";
  for (const auto& dependence : mapping.dependences()) {
    output << "  %e" << dependence.edge << " kind=" << kindName(dependence.kind)
           << " separation=" << dependence.requiredSeparationCycles;
    if (dependence.transport)
      output << " actions=" << dependence.transport->actions.size();
    output << '\n';
  }
  return output.str();
}

std::string toJson(const ModuloMapping& mapping) {
  Json root = {{"schema", "cgra.modulo_mapping.debug.v1"},
               {"target", mapping.targetName()},
               {"ii", mapping.ii()},
               {"placements", Json::array()},
               {"dependences", Json::array()}};
  for (const auto& placement : mapping.placements())
    root["placements"].push_back({{"node", placement.node},
                                  {"tile", tileJson(placement.tile)},
                                  {"slot", placement.issueSlot.value()}});
  for (const auto& dependence : mapping.dependences()) {
    Json value = {{"edge", dependence.edge},
                  {"kind", kindName(dependence.kind)},
                  {"required_separation_cycles", dependence.requiredSeparationCycles}};
    if (dependence.transport) {
      value["transport"] = {{"domain", domainName(dependence.transport->domain)},
                            {"actions", Json::array()}};
      for (const auto& action : dependence.transport->actions)
        value["transport"]["actions"].push_back(actionJson(action));
    }
    root["dependences"].push_back(std::move(value));
  }
  return root.dump(2) + '\n';
}

ModuloMapping parse(std::string_view jsonText) {
  Json root;
  try {
    root = Json::parse(jsonText);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("cannot parse modulo mapping JSON: ") + error.what());
  }
  if (!root.is_object() || root.value("schema", "") != "cgra.modulo_mapping.debug.v1")
    throw std::invalid_argument("mapping JSON schema must be cgra.modulo_mapping.debug.v1");
  ModuloMappingBuilder builder(required<std::string>(root, "target"),
                               required<std::uint32_t>(root, "ii"));
  const auto& placements = root.at("placements");
  if (!placements.is_array())
    throw std::invalid_argument("mapping placements must be an array");
  for (const auto& value : placements)
    builder.place(required<cgra::target::TargetNodeId>(value, "node"), parseTile(value.at("tile")),
                  ModuloSlot(required<std::uint32_t>(value, "slot")));
  const auto& dependences = root.at("dependences");
  if (!dependences.is_array())
    throw std::invalid_argument("mapping dependences must be an array");
  for (const auto& value : dependences) {
    const auto edge = required<cgra::target::TargetEdgeId>(value, "edge");
    const auto kind = kindFromName(required<std::string>(value, "kind"));
    const auto separation = required<std::uint32_t>(value, "required_separation_cycles");
    if (kind == cgra::ir::Edge::Kind::Memory) {
      if (value.contains("transport"))
        throw std::invalid_argument("memory mapping dependence cannot contain transport");
      builder.setMemorySeparation(edge, separation);
      continue;
    }
    if (!value.contains("transport"))
      throw std::invalid_argument("mapping data dependence is missing transport");
    const auto& transport = value.at("transport");
    if (!transport.is_object())
      throw std::invalid_argument("mapping transport must be an object");
    const auto domain = domainFromName(required<std::string>(transport, "domain"));
    const auto expectedKind = domain == NetworkDomain::Predicate ? cgra::ir::Edge::Kind::Predicate
                                                                 : cgra::ir::Edge::Kind::Data;
    if (kind != expectedKind)
      throw std::invalid_argument("mapping dependence kind does not match transport domain");
    if (!transport.contains("actions") || !transport.at("actions").is_array())
      throw std::invalid_argument("mapping transport actions must be an array");
    TransportPlan plan{edge, domain, {}, separation};
    for (const auto& action : transport.at("actions"))
      plan.actions.push_back(parseAction(action));
    builder.setTransport(edge, std::move(plan));
  }
  return builder.finish();
}

void writeJson(const ModuloMapping& mapping, const std::filesystem::path& path) {
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot write modulo mapping JSON: " + path.string());
  stream << toJson(mapping);
}

ModuloMapping readJson(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot read modulo mapping JSON: " + path.string());
  std::ostringstream contents;
  contents << stream.rdbuf();
  return parse(contents.str());
}

} // namespace cgra::mapping
