// SPDX-License-Identifier: MIT
#include "cgra/Schedule/StagedMappingSerialization.h"

#include "cgra/Mapping/ModuloMappingSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cgra::schedule {
namespace {

using Json = nlohmann::json;

template <typename T> T required(const Json& object, const char* key) {
  if (!object.contains(key))
    throw std::invalid_argument(std::string("staged mapping JSON missing ") + key);
  try {
    return object.at(key).get<T>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("staged mapping JSON field ") + key +
                                " has invalid type: " + error.what());
  }
}

} // namespace

std::string StagedMappingSerialization::dump(const StagedMapping& mapping) {
  std::ostringstream output;
  output << "StagedMapping target=\"" << mapping.modulo().targetName()
         << "\" II=" << mapping.modulo().ii() << " max_stage=" << mapping.maxStage() << "\n";
  for (const auto& entry : mapping.stages())
    output << "  %n" << entry.node << " -> stage " << entry.stage << "\n";
  return output.str();
}

std::string StagedMappingSerialization::toJson(const StagedMapping& mapping) {
  Json root = {{"schema", "cgra.staged_mapping.debug.v1"},
               {"target", mapping.modulo().targetName()},
               {"ii", mapping.modulo().ii()},
               {"modulo_mapping", Json::parse(cgra::mapping::toJson(mapping.modulo()))},
               {"stages", Json::array()}};
  for (const auto& entry : mapping.stages())
    root["stages"].push_back({{"node", entry.node}, {"stage", entry.stage}});
  return root.dump(2);
}

StagedMapping StagedMappingSerialization::parse(std::string_view jsonText) {
  Json root;
  try {
    root = Json::parse(jsonText);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid staged mapping JSON: ") + error.what());
  }
  if (required<std::string>(root, "schema") != "cgra.staged_mapping.debug.v1")
    throw std::invalid_argument("unsupported staged mapping schema");
  if (!root.contains("modulo_mapping") || !root.at("modulo_mapping").is_object())
    throw std::invalid_argument("staged mapping JSON requires modulo_mapping object");
  const auto modulo = cgra::mapping::parse(root.at("modulo_mapping").dump());
  if (required<std::uint32_t>(root, "ii") != modulo.ii() ||
      required<std::string>(root, "target") != modulo.targetName())
    throw std::invalid_argument("staged mapping metadata disagrees with embedded modulo mapping");
  if (!root.at("stages").is_array())
    throw std::invalid_argument("staged mapping stages must be an array");
  std::vector<NodeStage> stages;
  stages.reserve(root.at("stages").size());
  for (const auto& entry : root.at("stages"))
    stages.push_back({required<cgra::target::TargetNodeId>(entry, "node"),
                      required<PipelineStage>(entry, "stage")});
  return StagedMapping(modulo, std::move(stages));
}

void StagedMappingSerialization::writeJson(const StagedMapping& mapping,
                                           const std::filesystem::path& path) {
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("cannot open staged mapping output: " + path.string());
  output << toJson(mapping) << '\n';
}

StagedMapping StagedMappingSerialization::readJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open staged mapping input: " + path.string());
  std::ostringstream content;
  content << input.rdbuf();
  return parse(content.str());
}

std::string dump(const StagedMapping& mapping) { return StagedMappingSerialization::dump(mapping); }

std::string toJson(const StagedMapping& mapping) {
  return StagedMappingSerialization::toJson(mapping);
}

StagedMapping parse(std::string_view jsonText) {
  return StagedMappingSerialization::parse(jsonText);
}

void writeJson(const StagedMapping& mapping, const std::filesystem::path& path) {
  StagedMappingSerialization::writeJson(mapping, path);
}

StagedMapping readJson(const std::filesystem::path& path) {
  return StagedMappingSerialization::readJson(path);
}

} // namespace cgra::schedule
