// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetDFGSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace cgra::target {
namespace {

using Json = nlohmann::json;

Json typeJson(const ir::ValueType& type) {
  return {{"kind", ir::toString(type.kind)}, {"bits", type.bitWidth}};
}

ir::ValueType parseType(const Json& json) {
  if (json.is_string())
    return ir::ValueType::fromString(json.get<std::string>());
  if (!json.is_object() || !json.contains("kind") || !json.contains("bits"))
    throw std::invalid_argument("target DFG type must contain kind and bits");
  const auto kind = ir::valueKindFromString(json.at("kind").get<std::string>());
  return {kind, json.at("bits").get<std::uint16_t>()};
}

template <typename T> T required(const Json& json, std::string_view key) {
  const std::string name(key);
  if (!json.contains(name))
    throw std::invalid_argument("target DFG JSON missing " + name);
  return json.at(name).get<T>();
}

Json edgeJson(const TargetEdge& edge) {
  Json value = {{"id", edge.id}, {"src", edge.src}, {"dst", edge.dst}, {"distance", edge.distance}};
  if (edge.kind() == ir::Edge::Kind::Data) {
    value["kind"] = "data";
    value["operand"] = std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
  } else if (edge.kind() == ir::Edge::Kind::Predicate) {
    value["kind"] = "predicate";
    value["operand"] = std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
  } else {
    value["kind"] = "memory";
    value["dependence"] = ir::toString(std::get<ir::MemoryEdgeInfo>(edge.info).dependence);
  }
  return value;
}

} // namespace

std::string dump(const TargetDFG& dfg) {
  std::ostringstream output;
  output << "TargetDFG target=\"" << dfg.targetName() << "\" name=\"" << dfg.name() << "\"\n\n";
  for (const auto& node : dfg.nodes()) {
    output << "%n" << node.id << " [orig=";
    for (std::size_t index = 0; index < node.genericOrigins.size(); ++index) {
      if (index)
        output << ",";
      output << "%g" << node.genericOrigins[index];
    }
    output << "] = " << toString(node.executionClass) << "." << node.operation
           << " result=" << node.resultType.toString() << " issue=" << node.issueOccupancy
           << " latency=";
    if (node.resultLatency)
      output << *node.resultLatency;
    else
      output << "none";
    output << "\n";
  }
  output << "\nedges:\n";
  for (const auto& edge : dfg.edges()) {
    output << "%e" << edge.id << ": %n" << edge.src << " -> %n" << edge.dst;
    if (edge.kind() == ir::Edge::Kind::Memory)
      output << " kind=memory dependence="
             << ir::toString(std::get<ir::MemoryEdgeInfo>(edge.info).dependence);
    else if (edge.kind() == ir::Edge::Kind::Data)
      output << " kind=data operand=" << std::get<ir::DataEdgeInfo>(edge.info).dstOperand;
    else
      output << " kind=predicate operand=" << std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand;
    output << " distance=" << edge.distance << '\n';
  }
  return output.str();
}

std::string toJson(const TargetDFG& dfg) {
  Json root = {{"schema", "cgra.target_dfg.debug.v1"},
               {"target", dfg.targetName()},
               {"name", dfg.name()},
               {"external_values", Json::array()},
               {"constants", Json::array()},
               {"live_outs", Json::array()},
               {"nodes", Json::array()},
               {"operand_bindings", Json::array()},
               {"edges", Json::array()}};
  for (const auto& value : dfg.externalValues())
    root["external_values"].push_back(
        {{"id", value.id}, {"name", value.name}, {"type", typeJson(value.type)}});
  for (const auto& value : dfg.constants())
    root["constants"].push_back(
        {{"id", value.id}, {"bits", value.bits}, {"type", typeJson(value.type)}});
  for (const auto& value : dfg.liveOuts())
    root["live_outs"].push_back({{"id", value.id},
                                 {"name", value.name},
                                 {"type", typeJson(value.type)},
                                 {"node", value.source}});
  for (const auto& node : dfg.nodes()) {
    Json value = {{"id", node.id},
                  {"operation", node.operation},
                  {"execution_class", toString(node.executionClass)},
                  {"result_type", typeJson(node.resultType)},
                  {"operand_types", Json::array()},
                  {"issue_occupancy", node.issueOccupancy},
                  {"generic_origins", node.genericOrigins}};
    if (node.resultLatency)
      value["result_latency"] = *node.resultLatency;
    else
      value["result_latency"] = nullptr;
    for (const auto& type : node.operandTypes)
      value["operand_types"].push_back(typeJson(type));
    root["nodes"].push_back(std::move(value));
  }
  for (const auto& binding : dfg.operandBindings()) {
    Json value = {{"node", binding.node}, {"operand", binding.operand}};
    std::visit(
        [&](const auto& source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, ir::ExternalValueRef>)
            value["external"] = source.value;
          else
            value["constant"] = source.value;
        },
        binding.source);
    root["operand_bindings"].push_back(std::move(value));
  }
  for (const auto& edge : dfg.edges())
    root["edges"].push_back(edgeJson(edge));
  return root.dump(2) + '\n';
}

TargetDFG parse(std::string_view jsonText) {
  Json root;
  try {
    root = Json::parse(jsonText);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("cannot parse target DFG JSON: ") + error.what());
  }
  if (!root.is_object() || root.value("schema", "") != "cgra.target_dfg.debug.v1")
    throw std::invalid_argument("target DFG JSON schema must be cgra.target_dfg.debug.v1");
  TargetDFGBuilder builder(required<std::string>(root, "name"),
                           required<std::string>(root, "target"));
  for (const auto& value : root.at("external_values"))
    builder.addExternal({required<ir::ExternalValueId>(value, "id"), parseType(value.at("type")),
                         required<std::string>(value, "name")});
  for (const auto& value : root.at("constants"))
    builder.addConstant({required<ir::ConstantId>(value, "id"), parseType(value.at("type")),
                         required<std::uint64_t>(value, "bits")});
  for (const auto& value : root.at("nodes")) {
    TargetNode node;
    node.id = required<TargetNodeId>(value, "id");
    node.operation = required<std::string>(value, "operation");
    node.executionClass =
        targetExecutionClassFromString(required<std::string>(value, "execution_class"));
    node.resultType = parseType(value.at("result_type"));
    node.issueOccupancy = required<unsigned>(value, "issue_occupancy");
    if (!value.at("result_latency").is_null())
      node.resultLatency = value.at("result_latency").get<unsigned>();
    for (const auto& type : value.at("operand_types"))
      node.operandTypes.push_back(parseType(type));
    node.genericOrigins = value.at("generic_origins").get<std::vector<ir::NodeId>>();
    builder.addNode(std::move(node));
  }
  for (const auto& binding : root.at("operand_bindings")) {
    TargetOperandBinding result{
        required<TargetNodeId>(binding, "node"), required<std::uint32_t>(binding, "operand"), {}};
    if (binding.contains("external"))
      result.source = ir::ExternalValueRef{required<ir::ExternalValueId>(binding, "external")};
    else
      result.source = ir::ConstantRef{required<ir::ConstantId>(binding, "constant")};
    builder.addBinding(std::move(result));
  }
  for (const auto& value : root.at("live_outs"))
    builder.addLiveOut({required<ir::LiveOutId>(value, "id"), parseType(value.at("type")),
                        required<std::string>(value, "name"),
                        required<TargetNodeId>(value, "node")});
  for (const auto& value : root.at("edges")) {
    TargetEdge edge{required<TargetEdgeId>(value, "id"), required<TargetNodeId>(value, "src"),
                    required<TargetNodeId>(value, "dst"),
                    required<std::uint32_t>(value, "distance"), ir::DataEdgeInfo{}};
    const auto kind = required<std::string>(value, "kind");
    if (kind == "data")
      edge.info = ir::DataEdgeInfo{required<std::uint32_t>(value, "operand")};
    else if (kind == "predicate")
      edge.info = ir::PredicateEdgeInfo{required<std::uint32_t>(value, "operand")};
    else if (kind == "memory")
      edge.info = ir::MemoryEdgeInfo{
          ir::memoryDepKindFromString(required<std::string>(value, "dependence"))};
    else
      throw std::invalid_argument("unknown target DFG edge kind: " + kind);
    builder.addEdge(std::move(edge));
  }
  return builder.finish();
}

void writeJson(const TargetDFG& dfg, const std::filesystem::path& path) {
  std::ofstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot write target DFG JSON: " + path.string());
  stream << toJson(dfg);
}

TargetDFG readJson(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot read target DFG JSON: " + path.string());
  std::ostringstream contents;
  contents << stream.rdbuf();
  return parse(contents.str());
}

} // namespace cgra::target
