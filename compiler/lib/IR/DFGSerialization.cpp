// SPDX-License-Identifier: MIT
#include "cgra/IR/DFGSerialization.h"

#include "cgra/IR/DFGBuilder.h"
#include "cgra/IR/Opcode.h"
#include "cgra/IR/ValueType.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace cgra::ir {
namespace {

using Json = nlohmann::json;

Json typeJson(const ValueType& type) {
  return {{"kind", toString(type.kind)}, {"bits", type.bitWidth}};
}

ValueType parseType(const Json& json, std::string_view context) {
  if (!json.is_object() || !json.contains("kind") || !json.contains("bits"))
    throw std::invalid_argument(std::string(context) + " must contain kind and bits");
  const auto kind = valueKindFromString(json.at("kind").get<std::string>());
  const auto bits = json.at("bits").get<std::uint16_t>();
  if ((kind == ValueKind::Void && bits != 0) || (kind != ValueKind::Void && bits == 0))
    throw std::invalid_argument(std::string(context) + " has inconsistent kind and width");
  if (kind == ValueKind::Predicate && bits != 1)
    throw std::invalid_argument(std::string(context) + " predicate width must be one");
  return {kind, bits};
}

template <typename T> T required(const Json& json, std::string_view key, std::string_view context) {
  const std::string keyString(key);
  if (!json.contains(keyString))
    throw std::invalid_argument(std::string(context) + " missing " + std::string(key));
  try {
    return json.at(keyString).get<T>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string(context) + "." + std::string(key) +
                                " has invalid type: " + error.what());
  }
}

Json nodeJson(const Node& node) {
  Json value = {{"id", node.id},
                {"opcode", toString(node.opcode)},
                {"result_type", typeJson(node.resultType)},
                {"operand_types", Json::array()}};
  for (const auto& type : node.operandTypes)
    value["operand_types"].push_back(typeJson(type));
  if (node.icmpPredicate)
    value["icmp_predicate"] = toString(*node.icmpPredicate);
  if (node.memoryInfo)
    value["memory"] = {{"access_width_bits", node.memoryInfo->accessWidthBits},
                       {"volatile", node.memoryInfo->isVolatile}};
  if (node.source)
    value["source"] = {{"label", node.source->label}};
  return value;
}

Json edgeJson(const Edge& edge) {
  Json value = {{"id", edge.id}, {"src", edge.src}, {"dst", edge.dst}, {"distance", edge.distance}};
  switch (edge.kind()) {
  case Edge::Kind::Data:
    value["kind"] = "data";
    value["operand"] = std::get<DataEdgeInfo>(edge.info).dstOperand;
    break;
  case Edge::Kind::Predicate:
    value["kind"] = "predicate";
    value["operand"] = std::get<PredicateEdgeInfo>(edge.info).dstOperand;
    break;
  case Edge::Kind::Memory:
    value["kind"] = "memory";
    value["dependence"] = toString(std::get<MemoryEdgeInfo>(edge.info).dependence);
    break;
  }
  return value;
}

} // namespace

std::string dump(const DFG& dfg) {
  std::ostringstream output;
  output << "DFG \"" << dfg.name() << "\"\n\n";
  output << "externals:\n";
  for (const auto& value : dfg.externalValues())
    output << "  %ext" << value.id << " : " << value.type.toString() << " \"" << value.name
           << "\"\n";
  output << "\nconstants:\n";
  for (const auto& value : dfg.constants())
    output << "  %c" << value.id << " : " << value.type.toString() << " = 0x" << std::hex
           << value.bits << std::dec << "\n";
  output << "\nnodes:\n";
  for (const auto& node : dfg.nodes()) {
    output << "  %n" << node.id << " = " << toString(node.opcode) << " (";
    bool first = true;
    for (const auto& binding : dfg.externalBindings()) {
      if (binding.node != node.id)
        continue;
      if (!first)
        output << ", ";
      first = false;
      output << "operand" << binding.operand << "=";
      std::visit(
          [&](const auto& source) {
            using Source = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, ExternalValueRef>)
              output << "%ext" << source.value;
            else
              output << "%c" << source.value;
          },
          binding.source);
    }
    output << ") : " << node.resultType.toString() << "\n";
  }
  output << "\nlive-outs:\n";
  for (const auto& liveOut : dfg.liveOuts())
    output << "  %out" << liveOut.id << " : " << liveOut.type.toString() << " \"" << liveOut.name
           << "\" = %n" << liveOut.source << "\n";
  output << "\nedges:\n";
  for (const auto& edge : dfg.edges()) {
    output << "  %e" << edge.id << ": %n" << edge.src << " -> %n" << edge.dst;
    if (edge.kind() == Edge::Kind::Memory)
      output << " kind=memory dependence="
             << toString(std::get<MemoryEdgeInfo>(edge.info).dependence);
    else if (edge.kind() == Edge::Kind::Data)
      output << " kind=data operand=" << std::get<DataEdgeInfo>(edge.info).dstOperand;
    else
      output << " kind=predicate operand=" << std::get<PredicateEdgeInfo>(edge.info).dstOperand;
    output << " distance=" << edge.distance << "\n";
  }
  return output.str();
}

DFG parse(std::string_view jsonText) {
  Json root;
  try {
    root = Json::parse(jsonText);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("cannot parse DFG JSON: ") + error.what());
  }
  if (!root.is_object() || root.value("schema", "") != "cgra.dfg.debug.v1")
    throw std::invalid_argument("DFG JSON schema must be cgra.dfg.debug.v1");

  DFGBuilder builder(required<std::string>(root, "name", "DFG"));
  const auto& externals = root.at("external_values");
  if (!externals.is_array())
    throw std::invalid_argument("DFG.external_values must be an array");
  for (std::size_t index = 0; index < externals.size(); ++index) {
    const auto& value = externals.at(index);
    if (required<ExternalValueId>(value, "id", "external value") != index)
      throw std::invalid_argument("external value IDs must be serialized in order");
    builder.addExternal(required<std::string>(value, "name", "external value"),
                        parseType(value.at("type"), "external value type"));
  }

  const auto& constants = root.at("constants");
  if (!constants.is_array())
    throw std::invalid_argument("DFG.constants must be an array");
  for (std::size_t index = 0; index < constants.size(); ++index) {
    const auto& value = constants.at(index);
    if (required<ConstantId>(value, "id", "constant") != index)
      throw std::invalid_argument("constant IDs must be serialized in order");
    builder.addConstant(parseType(value.at("type"), "constant type"),
                        required<std::uint64_t>(value, "bits", "constant"));
  }

  const auto& nodes = root.at("nodes");
  if (!nodes.is_array())
    throw std::invalid_argument("DFG.nodes must be an array");
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const auto& value = nodes.at(index);
    if (required<NodeId>(value, "id", "node") != index)
      throw std::invalid_argument("node IDs must be serialized in order");
    std::vector<ValueType> operandTypes;
    for (const auto& type : required<Json>(value, "operand_types", "node"))
      operandTypes.push_back(parseType(type, "node operand type"));
    std::optional<ICmpPredicate> predicate;
    if (value.contains("icmp_predicate"))
      predicate = icmpPredicateFromString(value.at("icmp_predicate").get<std::string>());
    std::optional<MemoryOpInfo> memory;
    if (value.contains("memory")) {
      const auto& memoryJson = value.at("memory");
      memory = MemoryOpInfo{required<std::uint32_t>(memoryJson, "access_width_bits", "node.memory"),
                            required<bool>(memoryJson, "volatile", "node.memory")};
    }
    std::optional<SourceInfo> source;
    if (value.contains("source"))
      source = SourceInfo{required<std::string>(value.at("source"), "label", "node.source")};
    builder.addNode(opcodeFromString(required<std::string>(value, "opcode", "node")),
                    std::move(operandTypes), parseType(value.at("result_type"), "node result"),
                    predicate, memory, source);
  }

  const auto liveOuts = root.value("live_outs", Json::array());
  if (!liveOuts.is_array())
    throw std::invalid_argument("DFG.live_outs must be an array");
  for (std::size_t index = 0; index < liveOuts.size(); ++index) {
    const auto& value = liveOuts.at(index);
    if (required<LiveOutId>(value, "id", "live-out") != index)
      throw std::invalid_argument("live-out IDs must be serialized in order");
    builder.addLiveOut(required<std::string>(value, "name", "live-out"),
                       parseType(value.at("type"), "live-out type"),
                       required<NodeId>(value, "node", "live-out"));
  }

  const auto& bindings = root.at("external_bindings");
  if (!bindings.is_array())
    throw std::invalid_argument("DFG.external_bindings must be an array");
  for (const auto& binding : bindings) {
    const auto node = required<NodeId>(binding, "node", "binding");
    const auto operand = required<std::uint32_t>(binding, "operand", "binding");
    if (binding.contains("external"))
      builder.bindExternal(node, operand,
                           required<ExternalValueId>(binding, "external", "binding"));
    else if (binding.contains("constant"))
      builder.bindConstant(node, operand, required<ConstantId>(binding, "constant", "binding"));
    else
      throw std::invalid_argument("binding must contain external or constant");
  }

  const auto& edges = root.at("edges");
  if (!edges.is_array())
    throw std::invalid_argument("DFG.edges must be an array");
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const auto& edge = edges.at(index);
    if (required<EdgeId>(edge, "id", "edge") != index)
      throw std::invalid_argument("edge IDs must be serialized in order");
    const auto src = required<NodeId>(edge, "src", "edge");
    const auto dst = required<NodeId>(edge, "dst", "edge");
    const auto distance = required<std::uint32_t>(edge, "distance", "edge");
    const auto kind = required<std::string>(edge, "kind", "edge");
    if (kind == "data")
      builder.addDataEdge(src, dst, required<std::uint32_t>(edge, "operand", "edge"), distance);
    else if (kind == "predicate")
      builder.addPredicateEdge(src, dst, required<std::uint32_t>(edge, "operand", "edge"),
                               distance);
    else if (kind == "memory") {
      if (edge.contains("operand"))
        throw std::invalid_argument("memory edges must not contain an operand field");
      builder.addMemoryEdge(
          src, dst, memoryDepKindFromString(required<std::string>(edge, "dependence", "edge")),
          distance);
    } else
      throw std::invalid_argument("unknown DFG edge kind: " + kind);
  }
  return builder.finish();
}

void writeJson(const DFG& dfg, const std::filesystem::path& path) {
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot write DFG JSON: " + path.string());
  Json root = {{"schema", "cgra.dfg.debug.v1"},
               {"name", dfg.name()},
               {"external_values", Json::array()},
               {"constants", Json::array()},
               {"nodes", Json::array()},
               {"live_outs", Json::array()},
               {"external_bindings", Json::array()},
               {"edges", Json::array()}};
  for (const auto& value : dfg.externalValues())
    root["external_values"].push_back(
        {{"id", value.id}, {"name", value.name}, {"type", typeJson(value.type)}});
  for (const auto& value : dfg.constants())
    root["constants"].push_back(
        {{"id", value.id}, {"type", typeJson(value.type)}, {"bits", value.bits}});
  for (const auto& value : dfg.liveOuts())
    root["live_outs"].push_back({{"id", value.id},
                                 {"name", value.name},
                                 {"type", typeJson(value.type)},
                                 {"node", value.source}});
  for (const auto& node : dfg.nodes())
    root["nodes"].push_back(nodeJson(node));
  for (const auto& binding : dfg.externalBindings()) {
    Json value = {{"node", binding.node}, {"operand", binding.operand}};
    std::visit(
        [&](const auto& source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, ExternalValueRef>)
            value["external"] = source.value;
          else
            value["constant"] = source.value;
        },
        binding.source);
    root["external_bindings"].push_back(std::move(value));
  }
  for (const auto& edge : dfg.edges())
    root["edges"].push_back(edgeJson(edge));
  stream << root.dump(2) << '\n';
}

DFG readJson(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot read DFG JSON: " + path.string());
  std::ostringstream contents;
  contents << stream.rdbuf();
  return parse(contents.str());
}

} // namespace cgra::ir
