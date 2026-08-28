// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace cgra {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& context, const std::string& message) {
  throw std::runtime_error("target." + context + ": " + message);
}

const Json& requiredObject(const Json& parent, const std::string& key, const std::string& context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required object");
  const auto& value = parent.at(key);
  if (!value.is_object())
    fail(context + "." + key, "must be an object");
  return value;
}

const Json& requiredArray(const Json& parent, const std::string& key, const std::string& context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required array");
  const auto& value = parent.at(key);
  if (!value.is_array())
    fail(context + "." + key, "must be an array");
  return value;
}

template <typename T>
T required(const Json& parent, const std::string& key, const std::string& context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required value");
  try {
    return parent.at(key).get<T>();
  } catch (const Json::exception& error) {
    fail(context + "." + key, std::string("has invalid type: ") + error.what());
  }
}

unsigned positiveUnsigned(const Json& parent, const std::string& key, const std::string& context) {
  const auto value = required<std::uint64_t>(parent, key, context);
  if (value == 0)
    fail(context + "." + key, "must be greater than zero");
  if (value > std::numeric_limits<unsigned>::max())
    fail(context + "." + key, "does not fit unsigned");
  return static_cast<unsigned>(value);
}

SameAddressReadWritePolicy parseReadWritePolicy(const std::string& value,
                                                const std::string& context) {
  if (value == "illegal")
    return SameAddressReadWritePolicy::Forbidden;
  if (value == "read_old_then_write_new")
    return SameAddressReadWritePolicy::ReadOldThenWriteNew;
  if (value == "write_new_then_read")
    return SameAddressReadWritePolicy::WriteNewThenRead;
  fail(context, "unsupported same-address read/write policy");
}

RegisterFileDesc parseRegisterFile(const Json& root, const std::string& key,
                                   RegisterBankDomain domain, unsigned rows, unsigned cols) {
  const auto& json = requiredObject(root, key, "");
  RegisterFileDesc desc;
  desc.id = key;
  desc.domain = domain;
  desc.depth = positiveUnsigned(json, "depth", key);
  desc.readPorts = positiveUnsigned(json, "read_ports", key);
  desc.writePorts = positiveUnsigned(json, "write_ports", key);
  desc.sameCycleReadWriteSameAddress =
      required<std::string>(json, "same_cycle_read_write_same_address", key);
  desc.sameCycleMultiwriteSameAddress =
      required<std::string>(json, "same_cycle_multiwrite_same_address", key);
  desc.sameAddressReadWritePolicy = parseReadWritePolicy(
      desc.sameCycleReadWriteSameAddress, key + ".same_cycle_read_write_same_address");
  if (desc.sameCycleMultiwriteSameAddress != "illegal")
    fail(key + ".same_cycle_multiwrite_same_address", "unsupported policy");
  if (json.contains("allocatable_indices")) {
    const auto& indices = json.at("allocatable_indices");
    if (!indices.is_array() || indices.empty())
      fail(key + ".allocatable_indices", "must be a non-empty array");
    std::set<unsigned> seen;
    for (const auto& value : indices) {
      const auto index = value.get<unsigned>();
      if (index >= desc.depth)
        fail(key + ".allocatable_indices", "index is outside RF depth");
      if (!seen.insert(index).second)
        fail(key + ".allocatable_indices", "contains duplicate index");
      desc.allocatableIndices.push_back(index);
    }
  } else {
    desc.allocatableIndices.resize(desc.depth);
    std::iota(desc.allocatableIndices.begin(), desc.allocatableIndices.end(), 0U);
  }
  if (json.contains("applicable_tiles")) {
    const auto& tiles = json.at("applicable_tiles");
    if (!tiles.is_array() || tiles.empty())
      fail(key + ".applicable_tiles", "must be a non-empty array");
    std::set<std::pair<unsigned, unsigned>> seen;
    for (const auto& tile : tiles) {
      if (!tile.is_array() || tile.size() != 2)
        fail(key + ".applicable_tiles", "entries must be [row, col]");
      const auto row = tile.at(0).get<unsigned>();
      const auto col = tile.at(1).get<unsigned>();
      if (row >= rows || col >= cols)
        fail(key + ".applicable_tiles", "tile is outside the target array");
      if (!seen.emplace(row, col).second)
        fail(key + ".applicable_tiles", "contains duplicate tile");
      desc.applicableTiles.emplace_back(row, col);
    }
    std::ranges::sort(desc.applicableTiles);
  }
  const auto& ports = requiredObject(json, "write_ports_detail", key);
  for (const auto& [port, sources] : ports.items()) {
    if (!sources.is_object() || !sources.contains("sources") || !sources.at("sources").is_array() ||
        sources.at("sources").empty())
      fail(key + ".write_ports_detail." + port + ".sources", "must be a non-empty array");
    auto& portSources = desc.writePortSources[port];
    for (const auto& source : sources.at("sources")) {
      if (!source.is_string())
        fail(key + ".write_ports_detail." + port + ".sources", "entries must be strings");
      portSources.push_back(source.get<std::string>());
    }
  }
  if (desc.writePortSources.size() != desc.writePorts)
    fail(key + ".write_ports_detail", "must describe every register-file write port exactly once");
  return desc;
}

void requireEqual(unsigned lhs, unsigned rhs, const std::string& context) {
  if (lhs != rhs)
    fail(context, "inconsistent duplicate contract values");
}

void validateWritePortNames(const RegisterFileDesc& desc, const std::string& key) {
  if (desc.writePortSources.size() != desc.writePorts)
    fail(key + ".write_ports_detail", "must contain exactly the declared write ports");
  for (unsigned index = 0; index < desc.writePorts; ++index)
    if (!desc.writePortSources.contains("W" + std::to_string(index)))
      fail(key + ".write_ports_detail", "missing declared write port W" + std::to_string(index));
}

void validateWritePortSource(
    const RegisterFileDesc& desc, const std::string& key, std::string_view port,
    std::string_view sourceDomain,
    const std::unordered_map<std::string, std::unordered_map<std::string, unsigned>>& encodings) {
  const auto domainIt = encodings.find(std::string(sourceDomain));
  if (domainIt == encodings.end())
    fail("encodings." + std::string(sourceDomain),
         "missing encoding domain " + std::string(sourceDomain));
  std::unordered_set<std::string> seen;
  for (const auto& source : desc.writePortSources.at(std::string(port))) {
    if (!seen.insert(source).second)
      fail(key + ".write_ports_detail." + std::string(port) + ".sources",
           "contains duplicate source " + source);
    if (!domainIt->second.contains(source))
      fail(key + ".write_ports_detail." + std::string(port) + ".sources",
           "source " + source + " is not in encoding domain " + std::string(sourceDomain));
  }
}

void parseNetworkDomain(const Json& network, const std::string& key, InterconnectDesc& desc) {
  const auto& domain = requiredObject(network, key, "interconnect");
  desc.separateResourceDomain =
      required<bool>(domain, "separate_resource_domain", "interconnect." + key);
  desc.channelsPerDirectionPerLink =
      positiveUnsigned(domain, "channels_per_direction_per_link", "interconnect." + key);
  if (!desc.separateResourceDomain)
    fail("interconnect." + key + ".separate_resource_domain",
         "must be enabled for the data and predicate networks");
  if (desc.channelsPerDirectionPerLink != 1)
    fail("interconnect." + key + ".channels_per_direction_per_link",
         "only one compiler-routed channel is supported");
}

std::uint64_t tileKey(unsigned row, unsigned col) {
  return (static_cast<std::uint64_t>(row) << 32) | col;
}

bool dataValueType(const ir::ValueType& type) {
  return type.kind == ir::ValueKind::Integer || type.kind == ir::ValueKind::Float;
}

bool operandTypeMatchesRole(TargetOperandRole role, const ir::ValueType& type) {
  switch (role) {
  case TargetOperandRole::Data:
    return dataValueType(type);
  case TargetOperandRole::Predicate:
    return type == ir::ValueType::predicate();
  case TargetOperandRole::Address:
    return type.kind == ir::ValueKind::Integer;
  }
  return false;
}

bool resultTypeMatchesRole(TargetResultRole role, const ir::ValueType& type) {
  switch (role) {
  case TargetResultRole::Data:
    return dataValueType(type);
  case TargetResultRole::Predicate:
    return type == ir::ValueType::predicate();
  case TargetResultRole::Void:
    return type == ir::ValueType::voidTy();
  }
  return false;
}

} // namespace

TargetModel TargetModel::loadFromFile(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot open target contract: " + path.string());

  Json root;
  try {
    stream >> root;
  } catch (const Json::exception& error) {
    throw std::runtime_error("cannot parse target contract " + path.string() + ": " + error.what());
  }
  if (!root.is_object())
    fail("", "root must be an object");

  TargetModel model;
  const auto schema = required<std::string>(root, "schema", "");
  if (schema != "cgra.target.v2" && schema != "cgra.target.v3")
    fail("schema", "unsupported schema " + schema);
  const bool explicitLoweringContract = schema == "cgra.target.v3";
  model.contractVersion_ = positiveUnsigned(root, "target_contract_version", "");
  if (model.contractVersion_ != (explicitLoweringContract ? 3U : 2U))
    fail("target_contract_version",
         "unsupported contract version " + std::to_string(model.contractVersion_));
  model.name_ = required<std::string>(root, "name", "");
  if (model.name_.empty())
    fail("name", "must not be empty");
  if (root.contains("role"))
    model.role_ = required<std::string>(root, "role", "");
  if (model.role_ != "compiler_facing_target_contract" &&
      model.role_ != "mapping_research_target")
    fail("role", "must be compiler_facing_target_contract or mapping_research_target");
  const bool mappingResearchTarget = model.role_ == "mapping_research_target";
  if (root.contains("supported_value_types")) {
    const auto& types = requiredArray(root, "supported_value_types", "");
    for (std::size_t index = 0; index < types.size(); ++index) {
      if (!types[index].is_string())
        fail("supported_value_types[" + std::to_string(index) + "]", "must be a string");
      try {
        const auto type = ir::ValueType::fromString(types[index].get<std::string>());
        if (type.kind == ir::ValueKind::Void || type.kind == ir::ValueKind::Predicate)
          fail("supported_value_types[" + std::to_string(index) + "]",
               "void and predicate support are implicit");
        if (std::ranges::find(model.supportedValueTypes_, type) !=
            model.supportedValueTypes_.end())
          fail("supported_value_types", "contains a duplicate value type");
        model.supportedValueTypes_.push_back(type);
      } catch (const std::exception& error) {
        fail("supported_value_types[" + std::to_string(index) + "]", error.what());
      }
    }
  }
  if (mappingResearchTarget && model.supportedValueTypes_.empty())
    fail("supported_value_types", "mapping research target must declare its typed datapath");

  const auto& array = requiredObject(root, "array", "");
  model.array_.rows = positiveUnsigned(array, "rows", "array");
  model.array_.cols = positiveUnsigned(array, "cols", "array");
  model.array_.dataWidth = positiveUnsigned(array, "data_width", "array");
  model.array_.predicateWidth = positiveUnsigned(array, "predicate_width", "array");
  model.array_.hardwareBranch = required<bool>(array, "hardware_branch", "array");
  if (root.contains("parameters") && root.at("parameters").is_object() &&
      root.at("parameters").contains("const_mem_depth"))
    model.constantMemoryDepth_ =
        positiveUnsigned(root.at("parameters"), "const_mem_depth", "parameters");

  model.dataRF_ = parseRegisterFile(root, "data_rf", RegisterBankDomain::Data, model.array_.rows,
                                    model.array_.cols);
  model.predicateRF_ = parseRegisterFile(root, "predicate_rf", RegisterBankDomain::Predicate,
                                         model.array_.rows, model.array_.cols);

  const auto& network = requiredObject(root, "interconnect", "");
  model.interconnect_.topology = required<std::string>(network, "topology", "interconnect");
  model.interconnect_.registeredLinks = required<bool>(network, "registered_links", "interconnect");
  model.interconnect_.hopLatency = positiveUnsigned(network, "hop_latency", "interconnect");
  model.interconnect_.inputBuffering = required<bool>(network, "input_buffering", "interconnect");
  model.interconnect_.runtimeArbitration =
      required<bool>(network, "runtime_arbitration", "interconnect");
  model.interconnect_.compilerRouted = required<bool>(network, "compiler_routed", "interconnect");
  if (model.interconnect_.topology != "mesh_2d" && model.interconnect_.topology != "disconnected")
    fail("interconnect.topology", "unsupported topology");
  if (!model.interconnect_.registeredLinks || model.interconnect_.inputBuffering ||
      model.interconnect_.runtimeArbitration || !model.interconnect_.compilerRouted)
    fail("interconnect", "does not describe the compiler-controlled registered mesh");
  parseNetworkDomain(network, "data_network", model.interconnect_);
  InterconnectDesc predicateNetwork = model.interconnect_;
  parseNetworkDomain(network, "predicate_network", predicateNetwork);
  if (predicateNetwork.separateResourceDomain != model.interconnect_.separateResourceDomain ||
      predicateNetwork.channelsPerDirectionPerLink !=
          model.interconnect_.channelsPerDirectionPerLink)
    fail("interconnect", "data and predicate network resources disagree");

  const auto& memory = requiredObject(root, "memory", "");
  model.memory_.model = required<std::string>(memory, "model", "memory");
  model.memory_.addressUnit = required<std::string>(memory, "address_unit", "memory");
  model.memory_.depth = positiveUnsigned(memory, "depth", "memory");
  model.memory_.widthBits = positiveUnsigned(memory, "width_bits", "memory");
  model.memory_.ports = positiveUnsigned(memory, "ports", "memory");
  model.memory_.loadLatency = positiveUnsigned(memory, "load_latency", "memory");
  model.memory_.maxIssuePerLsuPerCycle =
      positiveUnsigned(memory, "max_issue_per_lsu_per_cycle", "memory");
  model.memory_.maxIssuePerPortPerCycle =
      positiveUnsigned(memory, "max_issue_per_port_per_cycle", "memory");
  const auto& separation = requiredObject(memory, "dependence_separation", "memory");
  model.memory_.rawDependenceSeparation =
      positiveUnsigned(separation, "RAW", "memory.dependence_separation");
  model.memory_.warDependenceSeparation =
      positiveUnsigned(separation, "WAR", "memory.dependence_separation");
  model.memory_.wawDependenceSeparation =
      positiveUnsigned(separation, "WAW", "memory.dependence_separation");
  model.memory_.sameAddressPolicy = required<std::string>(memory, "same_address_policy", "memory");
  model.memory_.runtimeStall = required<bool>(memory, "runtime_stall", "memory");
  model.memory_.runtimeArbitration = required<bool>(memory, "runtime_arbitration", "memory");
  if (model.memory_.model != "shared_multiport_scratchpad")
    fail("memory.model", "unsupported memory model");
  if (model.memory_.addressUnit != "word")
    fail("memory.address_unit", "must be word");
  if (model.memory_.sameAddressPolicy != "multi_load_only")
    fail("memory.same_address_policy", "unsupported policy");
  if (model.memory_.runtimeStall || model.memory_.runtimeArbitration)
    fail("memory", "runtime stall/arbitration must be disabled");

  // The legacy latency/op capability views remain part of the v2 contract for
  // control-word and RTL consistency checks. Operation semantics themselves
  // are reconstructed exclusively from the complete per-operation descriptors.
  const auto& latencies = requiredObject(root, "latencies", "");
  const auto& legacyFuLatencies = requiredObject(latencies, "fu_ops", "latencies");
  const auto& legacyOffsets =
      requiredObject(latencies, "producer_output_ready_offsets", "latencies");
  const auto& legacyOccupancies = requiredObject(latencies, "issue_occupancy", "latencies");
  static_cast<void>(
      required<unsigned>(legacyOffsets, "fu", "latencies.producer_output_ready_offsets"));
  static_cast<void>(
      required<unsigned>(legacyOffsets, "load", "latencies.producer_output_ready_offsets"));
  static_cast<void>(positiveUnsigned(legacyOccupancies, "fu", "latencies.issue_occupancy"));
  static_cast<void>(positiveUnsigned(legacyOccupancies, "load", "latencies.issue_occupancy"));
  static_cast<void>(positiveUnsigned(legacyOccupancies, "store", "latencies.issue_occupancy"));
  const auto& legacyOps = requiredObject(root, "ops", "");
  for (const auto& key : {"data", "compare", "predicate"}) {
    const auto& names = requiredArray(legacyOps, key, "ops");
    for (const auto& item : names) {
      if (!item.is_string())
        fail("ops." + std::string(key), "operation names must be strings");
      const auto name = item.get<std::string>();
      if (!legacyFuLatencies.contains(name))
        fail("latencies.fu_ops." + name, "missing latency for legacy operation");
    }
  }
  const auto& operationCompatibility = requiredObject(root, "operation_compatibility", "");
  if (required<std::string>(operationCompatibility, "status", "operation_compatibility") !=
          "legacy_compatibility_view" ||
      required<std::string>(operationCompatibility, "authoritative_section",
                            "operation_compatibility") != "operations")
    fail("operation_compatibility", "must identify operations as the authoritative section");

  const auto& operationJson = requiredObject(root, "operations", "");
  if (operationJson.empty())
    fail("operations", "must not be empty");
  for (const auto& [name, descriptorJson] : operationJson.items()) {
    const auto context = "operations." + name;
    if (name.empty())
      fail("operations", "operation names must not be empty");
    if (!descriptorJson.is_object())
      fail(context, "must be an object");
    TargetExecutionClass executionClass;
    try {
      executionClass = targetExecutionClassFromString(
          required<std::string>(descriptorJson, "execution_class", context));
    } catch (const std::exception& error) {
      fail(context + ".execution_class", error.what());
    }
    const auto& operandJson = requiredArray(descriptorJson, "operands", context);
    std::vector<TargetOperandDesc> operands;
    operands.reserve(operandJson.size());
    bool optionalSeen = false;
    for (std::size_t index = 0; index < operandJson.size(); ++index) {
      const auto operandContext = context + ".operands[" + std::to_string(index) + "]";
      if (!operandJson[index].is_object())
        fail(operandContext, "must be an object");
      TargetOperandDesc operand;
      try {
        operand.role = targetOperandRoleFromString(
            required<std::string>(operandJson[index], "role", operandContext));
      } catch (const std::exception& error) {
        fail(operandContext + ".role", error.what());
      }
      if (operandJson[index].contains("optional"))
        operand.optional = required<bool>(operandJson[index], "optional", operandContext);
      if (operandJson[index].contains("type") || operandJson[index].contains("types")) {
        const auto& typeJson = operandJson[index].contains("type")
                                   ? operandJson[index].at("type")
                                   : operandJson[index].at("types");
        const auto typeContext = operandContext + "." +
                                 (operandJson[index].contains("type") ? "type" : "types");
        try {
          if (typeJson.is_string()) {
            operand.acceptedTypes.push_back(
                ir::ValueType::fromString(typeJson.get<std::string>()));
          } else if (typeJson.is_array() && !typeJson.empty()) {
            for (const auto& value : typeJson) {
              if (!value.is_string())
                fail(typeContext, "entries must be strings");
              operand.acceptedTypes.push_back(ir::ValueType::fromString(value.get<std::string>()));
            }
          } else {
            fail(typeContext, "must be a type string or non-empty type array");
          }
        } catch (const std::exception& error) {
          fail(typeContext, error.what());
        }
        std::vector<ir::ValueType> uniqueTypes;
        for (const auto& type : operand.acceptedTypes) {
          if (std::ranges::find(uniqueTypes, type) != uniqueTypes.end())
            fail(typeContext, "contains a duplicate value type");
          uniqueTypes.push_back(type);
          if (!operandTypeMatchesRole(operand.role, type))
            fail(typeContext, "declared type is incompatible with operand role");
          if (mappingResearchTarget && type.kind != ir::ValueKind::Predicate &&
              std::ranges::find(model.supportedValueTypes_, type) ==
                  model.supportedValueTypes_.end())
            fail(typeContext, "declared type is not supported by the target datapath");
        }
      }
      if (operand.optional)
        optionalSeen = true;
      else if (optionalSeen)
        fail(operandContext, "required operand follows optional operand");
      operands.push_back(operand);
    }
    std::optional<TargetEncodingRef> encoding;
    if (descriptorJson.contains("encoding")) {
      const auto& encodingJson = requiredObject(descriptorJson, "encoding", context);
      encoding = TargetEncodingRef{
          required<std::string>(encodingJson, "domain", context + ".encoding"),
          required<std::string>(encodingJson, "symbol", context + ".encoding")};
      if (encoding->domain.empty() || encoding->symbol.empty())
        fail(context + ".encoding", "domain and symbol must not be empty");
    } else if (!mappingResearchTarget) {
      fail(context + ".encoding", "hardware target operation requires an encoding binding");
    }
    const auto& resultJson = requiredObject(descriptorJson, "result", context);
    TargetResultRole resultRole;
    try {
      resultRole = targetResultRoleFromString(
          required<std::string>(resultJson, "role", context + ".result"));
    } catch (const std::exception& error) {
      fail(context + ".result.role", error.what());
    }
    std::vector<ir::ValueType> acceptedResultTypes;
    if (resultJson.contains("type") || resultJson.contains("types")) {
      const auto& typeJson = resultJson.contains("type") ? resultJson.at("type")
                                                          : resultJson.at("types");
      const auto typeContext = context + ".result." +
                               (resultJson.contains("type") ? "type" : "types");
      try {
        if (typeJson.is_string()) {
          acceptedResultTypes.push_back(
              ir::ValueType::fromString(typeJson.get<std::string>()));
        } else if (typeJson.is_array() && !typeJson.empty()) {
          for (const auto& value : typeJson) {
            if (!value.is_string())
              fail(typeContext, "entries must be strings");
            acceptedResultTypes.push_back(ir::ValueType::fromString(value.get<std::string>()));
          }
        } else {
          fail(typeContext, "must be a type string or non-empty type array");
        }
      } catch (const std::exception& error) {
        fail(typeContext, error.what());
      }
      std::vector<ir::ValueType> uniqueTypes;
      for (const auto& type : acceptedResultTypes) {
        if (std::ranges::find(uniqueTypes, type) != uniqueTypes.end())
          fail(typeContext, "contains a duplicate value type");
        uniqueTypes.push_back(type);
        if (!resultTypeMatchesRole(resultRole, type))
          fail(typeContext, "declared type is incompatible with result role");
        if (mappingResearchTarget && type.kind != ir::ValueKind::Predicate &&
            type.kind != ir::ValueKind::Void &&
            std::ranges::find(model.supportedValueTypes_, type) ==
                model.supportedValueTypes_.end())
          fail(typeContext, "declared type is not supported by the target datapath");
      }
    }
    std::vector<std::pair<unsigned, TargetControlSink>> operandSinks;
    std::unordered_set<TargetControlSink> usedSinks;
    TargetResultSource resultSource = TargetResultSource::None;
    if (descriptorJson.contains("lowering")) {
      const auto& loweringJson = requiredObject(descriptorJson, "lowering", context);
      const auto& sinkJson = requiredArray(loweringJson, "operand_sinks", context + ".lowering");
      if (sinkJson.size() != operands.size())
        fail(context + ".lowering.operand_sinks", "must contain exactly one sink per operand");
      for (std::size_t index = 0; index < sinkJson.size(); ++index) {
        if (!sinkJson[index].is_string())
          fail(context + ".lowering.operand_sinks", "sink names must be strings");
        TargetControlSink sink;
        try {
          sink = targetControlSinkFromString(sinkJson[index].get<std::string>());
        } catch (const std::exception& error) {
          fail(context + ".lowering.operand_sinks[" + std::to_string(index) + "]", error.what());
        }
        if (!usedSinks.insert(sink).second)
          fail(context + ".lowering.operand_sinks", "each physical sink may be bound only once");
        operandSinks.emplace_back(static_cast<unsigned>(index), sink);
      }
      try {
        resultSource = targetResultSourceFromString(
            required<std::string>(loweringJson, "result_source", context + ".lowering"));
      } catch (const std::exception& error) {
        fail(context + ".lowering.result_source", error.what());
      }
    } else {
      if (explicitLoweringContract && !mappingResearchTarget)
        fail(context + ".lowering", "canonical v3 operation is missing explicit lowering");
      // Legacy target files predate the explicit lowering contract.  Derive the
      // canonical current-target sinks once at load time so all consumers still
      // use typed descriptors rather than operation-name checks.
      unsigned dataIndex = 0;
      unsigned predicateIndex = 0;
      for (std::size_t index = 0; index < operands.size(); ++index) {
        TargetControlSink sink;
        switch (operands[index].role) {
        case TargetOperandRole::Data:
          if (executionClass == TargetExecutionClass::LSU)
            sink = TargetControlSink::LsuStoreData;
          else
            sink = dataIndex++ == 0 ? TargetControlSink::FuDataA : TargetControlSink::FuDataB;
          break;
        case TargetOperandRole::Predicate:
          if (executionClass == TargetExecutionClass::LSU)
            sink = TargetControlSink::LsuCommitPredicate;
          else
            sink = predicateIndex++ == 0 ? TargetControlSink::FuPredicate0
                                         : TargetControlSink::FuPredicate1;
          break;
        case TargetOperandRole::Address:
          sink = TargetControlSink::LsuAddress;
          break;
        }
        operandSinks.emplace_back(static_cast<unsigned>(index), sink);
      }
      if (resultRole == TargetResultRole::Data)
        resultSource = executionClass == TargetExecutionClass::LSU
                           ? TargetResultSource::LsuLoadData
                           : TargetResultSource::FuDataResult;
      else if (resultRole == TargetResultRole::Predicate)
        resultSource = TargetResultSource::FuPredicateResult;
    }
    const auto issueOccupancy = positiveUnsigned(descriptorJson, "issue_occupancy", context);
    std::optional<unsigned> resultLatency;
    std::optional<unsigned> outputReadyOffset;
    if (descriptorJson.contains("result_latency"))
      resultLatency = positiveUnsigned(descriptorJson, "result_latency", context);
    if (descriptorJson.contains("producer_output_ready_offset"))
      outputReadyOffset =
          required<unsigned>(descriptorJson, "producer_output_ready_offset", context);
    std::optional<unsigned> accessWidth;
    if (descriptorJson.contains("memory_access_width_bits"))
      accessWidth = positiveUnsigned(descriptorJson, "memory_access_width_bits", context);
    const bool uniformDataType = descriptorJson.contains("uniform_data_type")
                                     ? required<bool>(descriptorJson, "uniform_data_type", context)
                                     : false;
    if (uniformDataType &&
        (resultRole != TargetResultRole::Data ||
         std::ranges::none_of(operands, [](const auto& operand) {
           return operand.role == TargetOperandRole::Data;
         })))
      fail(context + ".uniform_data_type",
           "requires a data result and at least one data operand");

    if (resultRole == TargetResultRole::Void) {
      if (resultLatency || outputReadyOffset)
        fail(context, "void operation must not define result latency/output readiness");
    } else if (!resultLatency || !outputReadyOffset) {
      fail(context, "value-producing operation requires result_latency and "
                    "producer_output_ready_offset");
    }
    if ((executionClass == TargetExecutionClass::LSU) != accessWidth.has_value())
      fail(context,
           "LSU operations require memory_access_width_bits and FU operations must omit it");
    for (const auto& [operandIndex, sink] : operandSinks) {
      const auto role = operands[operandIndex].role;
      const bool valid =
          (role == TargetOperandRole::Data &&
           (sink == TargetControlSink::FuDataA || sink == TargetControlSink::FuDataB ||
            sink == TargetControlSink::LsuStoreData)) ||
          (role == TargetOperandRole::Predicate &&
           (sink == TargetControlSink::FuPredicate0 || sink == TargetControlSink::FuPredicate1 ||
            sink == TargetControlSink::LsuCommitPredicate)) ||
          (role == TargetOperandRole::Address && sink == TargetControlSink::LsuAddress);
      if (!valid)
        fail(context + ".lowering.operand_sinks[" + std::to_string(operandIndex) + "]",
             "sink is incompatible with semantic operand role");
      const bool lsuSink = sink == TargetControlSink::LsuAddress ||
                           sink == TargetControlSink::LsuStoreData ||
                           sink == TargetControlSink::LsuCommitPredicate;
      if ((executionClass == TargetExecutionClass::LSU) != lsuSink)
        fail(context + ".lowering.operand_sinks[" + std::to_string(operandIndex) + "]",
             "sink is incompatible with operation execution class");
    }
    const bool validResultSource =
        (resultRole == TargetResultRole::Void && resultSource == TargetResultSource::None) ||
        (resultRole == TargetResultRole::Data && executionClass == TargetExecutionClass::FU &&
         resultSource == TargetResultSource::FuDataResult) ||
        (resultRole == TargetResultRole::Predicate && executionClass == TargetExecutionClass::FU &&
         resultSource == TargetResultSource::FuPredicateResult) ||
        (resultRole == TargetResultRole::Data && executionClass == TargetExecutionClass::LSU &&
         resultSource == TargetResultSource::LsuLoadData);
    if (!validResultSource)
      fail(context + ".lowering.result_source",
           "result source is incompatible with result role and execution class");
    const auto defaultResultType =
        resultRole == TargetResultRole::Data
            ? ir::ValueType::integer(model.array_.dataWidth)
            : resultRole == TargetResultRole::Predicate ? ir::ValueType::predicate()
                                                        : ir::ValueType::voidTy();
    TargetOperationDesc operation{
        name, executionClass, std::move(operands), resultRole,
        acceptedResultTypes.empty() ? defaultResultType : acceptedResultTypes.front(),
        issueOccupancy, resultLatency,
        outputReadyOffset, accessWidth, encoding, std::move(operandSinks), resultSource,
        std::move(acceptedResultTypes), uniformDataType};
    model.operations_.push_back(std::move(operation));
    model.operationIndices_.emplace(name, model.operations_.size() - 1);
  }
  for (const auto& operation : model.operations_)
    if (operation.executionClass == TargetExecutionClass::FU)
      model.defaultFuOperations_.insert(operation.id);
  if (root.contains("tile_capabilities")) {
    const auto& capabilities = requiredObject(root, "tile_capabilities", "");
    const auto& defaults =
        requiredArray(capabilities, "default_fu_operations", "tile_capabilities");
    model.defaultFuOperations_.clear();
    for (const auto& item : defaults) {
      if (!item.is_string())
        fail("tile_capabilities.default_fu_operations", "operation names must be strings");
      const auto name = item.get<std::string>();
      const auto* operation = model.findOperation(name);
      if (!operation || operation->executionClass != TargetExecutionClass::FU)
        fail("tile_capabilities.default_fu_operations", "operation is not a FU operation: " + name);
      if (!model.defaultFuOperations_.insert(name).second)
        fail("tile_capabilities.default_fu_operations", "duplicate operation: " + name);
    }
    const auto& overrides = requiredArray(capabilities, "overrides", "tile_capabilities");
    for (const auto& item : overrides) {
      if (!item.is_object())
        fail("tile_capabilities.overrides", "entries must be objects");
      const auto row = required<unsigned>(item, "row", "tile_capabilities.overrides");
      const auto col = required<unsigned>(item, "col", "tile_capabilities.overrides");
      if (row >= model.array_.rows || col >= model.array_.cols)
        fail("tile_capabilities.overrides", "tile coordinate is outside the array");
      const auto key = tileKey(row, col);
      if (model.tileOperationOverrides_.contains(key))
        fail("tile_capabilities.overrides", "duplicate tile override");
      const auto& operationJson = requiredArray(item, "operations", "tile_capabilities.overrides");
      auto& supported = model.tileOperationOverrides_[key];
      for (const auto& operationValue : operationJson) {
        if (!operationValue.is_string())
          fail("tile_capabilities.overrides.operations", "operation names must be strings");
        const auto name = operationValue.get<std::string>();
        const auto* operation = model.findOperation(name);
        if (!operation || operation->executionClass != TargetExecutionClass::FU)
          fail("tile_capabilities.overrides.operations",
               "operation is not a FU operation: " + name);
        if (!supported.insert(name).second)
          fail("tile_capabilities.overrides.operations", "duplicate operation: " + name);
      }
    }
  }
  const auto* loadOperationPtr = model.findOperation("LOAD");
  if (!loadOperationPtr)
    fail("operations.LOAD", "required operation descriptor is missing");
  const auto& loadOperation = *loadOperationPtr;
  if (loadOperation.executionClass != TargetExecutionClass::LSU ||
      loadOperation.resultType != ir::ValueType::integer(model.memory_.widthBits) ||
      loadOperation.accessWidthBits != model.memory_.widthBits)
    fail("operations.LOAD", "does not match memory width/execution contract");
  const auto* storeOperationPtr = model.findOperation("STORE");
  if (!storeOperationPtr)
    fail("operations.STORE", "required operation descriptor is missing");
  const auto& storeOperation = *storeOperationPtr;
  if (storeOperation.executionClass != TargetExecutionClass::LSU ||
      storeOperation.resultType != ir::ValueType::voidTy() ||
      storeOperation.accessWidthBits != model.memory_.widthBits)
    fail("operations.STORE", "does not match memory width/execution contract");

  // Legacy RTL/control views are validated for shape above, but do not define
  // compiler operation semantics. The complete per-operation descriptors are
  // authoritative and may evolve independently of these compatibility views.
  for (const auto& [name, latencyJson] : legacyFuLatencies.items()) {
    if (!latencyJson.is_number_unsigned() && !latencyJson.is_number_integer())
      fail("latencies.fu_ops." + name, "must be an unsigned integer");
    if (latencyJson.get<std::int64_t>() < 0)
      fail("latencies.fu_ops." + name, "must not be negative");
  }

  const auto& loop = requiredObject(root, "loop_execution", "");
  model.loopExecution_.supported = required<bool>(loop, "supported", "loop_execution");
  model.loopExecution_.model = required<std::string>(loop, "model", "loop_execution");
  model.loopExecution_.controlPhases =
      required<std::vector<std::string>>(loop, "control_phases", "loop_execution");
  model.loopExecution_.rotatingRegisters =
      required<bool>(loop, "rotating_registers", "loop_execution");
  model.loopExecution_.loopCounterOperand =
      required<bool>(loop, "loop_counter_operand", "loop_execution");
  model.loopExecution_.sameAddressRfReadWriteRecurrence =
      required<bool>(loop, "same_address_rf_read_write_recurrence", "loop_execution");
  if (!model.loopExecution_.supported || model.loopExecution_.model != "finite_modulo_replay")
    fail("loop_execution", "unsupported loop execution model");
  if (model.loopExecution_.controlPhases !=
      std::vector<std::string>{"prologue", "kernel", "epilogue"})
    fail("loop_execution.control_phases", "must be prologue, kernel, epilogue");
  if (model.loopExecution_.rotatingRegisters || model.loopExecution_.loopCounterOperand ||
      model.loopExecution_.sameAddressRfReadWriteRecurrence)
    fail("loop_execution", "unsupported loop resources are enabled");

  const auto& encodings = requiredObject(root, "encodings", "");
  for (const auto& [domain, entries] : encodings.items()) {
    if (!entries.is_object() || entries.empty())
      fail("encodings." + domain, "must be a non-empty object");
    std::unordered_set<unsigned> seenValues;
    for (const auto& [name, jsonValue] : entries.items()) {
      if (!jsonValue.is_number_unsigned() && !jsonValue.is_number_integer())
        fail("encodings." + domain + "." + name, "must be an unsigned integer");
      const auto signedValue = jsonValue.get<std::int64_t>();
      if (signedValue < 0 ||
          static_cast<std::uint64_t>(signedValue) > std::numeric_limits<unsigned>::max())
        fail("encodings." + domain + "." + name, "does not fit unsigned");
      const auto value = static_cast<unsigned>(signedValue);
      if (!seenValues.insert(value).second)
        fail("encodings." + domain, "duplicate numeric value " + std::to_string(value));
      model.encodings_[domain].emplace(name, value);
      model.reverseEncodings_[domain].emplace(value, name);
    }
  }

  for (const auto& operation : model.operations_) {
    if (!operation.encoding) {
      if (mappingResearchTarget)
        continue;
      fail("operations." + operation.id + ".encoding", "missing encoding binding");
    }
    const auto& binding = *operation.encoding;
    const auto domainIt = model.encodings_.find(binding.domain);
    if (domainIt == model.encodings_.end())
      fail("operations." + operation.id + ".encoding.domain",
           "references missing encoding domain " + binding.domain);
    if (!domainIt->second.contains(binding.symbol))
      fail("operations." + operation.id + ".encoding.symbol",
           "references missing encoding " + binding.domain + "." + binding.symbol);
    if (operation.executionClass == TargetExecutionClass::LSU && binding.domain != "lsu_op")
      fail("operations." + operation.id + ".encoding.domain",
           "LSU operations must bind the lsu_op encoding domain");
    if (operation.executionClass == TargetExecutionClass::FU && binding.domain == "lsu_op")
      fail("operations." + operation.id + ".encoding.domain",
           "FU operations must not bind the lsu_op encoding domain");
  }

  validateWritePortNames(model.dataRF_, "data_rf");
  validateWritePortNames(model.predicateRF_, "predicate_rf");
  validateWritePortSource(model.dataRF_, "data_rf", "W0", "route_data_source", model.encodings_);
  if (model.dataRF_.writePorts > 1)
    validateWritePortSource(model.dataRF_, "data_rf", "W1", "data_source", model.encodings_);
  validateWritePortSource(model.predicateRF_, "predicate_rf", "W0", "route_predicate_source",
                          model.encodings_);
  if (model.predicateRF_.writePorts > 1)
    validateWritePortSource(model.predicateRF_, "predicate_rf", "W1", "predicate_source",
                            model.encodings_);

  const auto& layout = requiredObject(root, "control_layout", "");
  auto& parsedLayout = model.controlLayout_;
  parsedLayout.rawWidth_ = positiveUnsigned(layout, "raw_width", "control_layout");
  parsedLayout.physicalWidth_ = positiveUnsigned(layout, "physical_width", "control_layout");
  parsedLayout.chunks_ = positiveUnsigned(layout, "chunks", "control_layout");
  parsedLayout.chunkBits_ = positiveUnsigned(layout, "chunk_bits", "control_layout");
  parsedLayout.chunkOrder_ = required<std::string>(layout, "chunk_order", "control_layout");
  if (parsedLayout.physicalWidth_ < parsedLayout.rawWidth_)
    fail("control_layout.physical_width", "must be at least raw_width");
  if (parsedLayout.chunks_ != 4 || parsedLayout.chunkBits_ != 32)
    fail("control_layout", "EncodedControl requires exactly four 32-bit chunks");
  if (parsedLayout.chunks_ * parsedLayout.chunkBits_ != parsedLayout.physicalWidth_)
    fail("control_layout", "chunks * chunk_bits must equal physical_width");
  if (parsedLayout.chunkOrder_ != "little_word")
    fail("control_layout.chunk_order", "unsupported chunk order");

  const auto& padding = requiredObject(layout, "padding", "control_layout");
  parsedLayout.paddingLsb_ = required<unsigned>(padding, "lsb", "control_layout.padding");
  parsedLayout.paddingWidth_ = required<unsigned>(padding, "width", "control_layout.padding");
  parsedLayout.paddingValue_ = required<std::uint64_t>(padding, "value", "control_layout.padding");
  if (parsedLayout.paddingLsb_ != parsedLayout.rawWidth_ ||
      parsedLayout.paddingWidth_ != parsedLayout.physicalWidth_ - parsedLayout.rawWidth_)
    fail("control_layout.padding", "must cover exactly physical bits above raw_width");
  if (parsedLayout.paddingValue_ != 0)
    fail("control_layout.padding.value", "only zero padding is supported");

  std::vector<bool> covered(parsedLayout.rawWidth_, false);
  const auto& fields = requiredArray(layout, "fields", "control_layout");
  if (fields.empty())
    fail("control_layout.fields", "must not be empty");
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& fieldJson = fields[index];
    const auto context = "control_layout.fields[" + std::to_string(index) + "]";
    if (!fieldJson.is_object())
      fail(context, "must be an object");
    ControlField field;
    field.name = required<std::string>(fieldJson, "name", context);
    field.lsb = required<unsigned>(fieldJson, "lsb", context);
    field.width = positiveUnsigned(fieldJson, "width", context);
    if (field.name.empty())
      fail(context + ".name", "must not be empty");
    if (field.lsb > parsedLayout.rawWidth_ || field.width > parsedLayout.rawWidth_ - field.lsb)
      fail(context, "extends beyond raw_width");
    if (parsedLayout.fieldIndices_.contains(field.name))
      fail(context + ".name", "duplicate field " + field.name);
    for (unsigned bit = field.lsb; bit < field.lsb + field.width; ++bit) {
      if (covered[bit])
        fail(context, "overlaps another field at bit " + std::to_string(bit));
      covered[bit] = true;
    }
    if (fieldJson.contains("encoding")) {
      field.encoding = required<std::string>(fieldJson, "encoding", context);
      const auto domain = model.encodings_.find(*field.encoding);
      if (domain == model.encodings_.end())
        fail(context + ".encoding", "references missing encoding domain " + *field.encoding);
      const std::uint64_t limit = field.width >= 64 ? std::numeric_limits<std::uint64_t>::max()
                                                    : (std::uint64_t{1} << field.width) - 1;
      for (const auto& [name, value] : domain->second) {
        if (field.width < 64 && value > limit)
          fail("encodings." + *field.encoding + "." + name,
               "value does not fit field " + field.name);
      }
    }
    parsedLayout.fieldIndices_.emplace(field.name, parsedLayout.fields_.size());
    parsedLayout.fields_.push_back(std::move(field));
  }
  if (std::ranges::find(covered, false) != covered.end())
    fail("control_layout.fields", "must cover every raw control bit exactly once");
  if (model.constantMemoryDepth_ == 0)
    model.constantMemoryDepth_ = 1U << parsedLayout.field("constantAddr").width;

  const auto& lsu = requiredObject(root, "lsu", "");
  const auto portAssignment = required<std::string>(lsu, "port_assignment", "lsu");
  if (portAssignment != "enabled_tile_row_major_rank")
    fail("lsu.port_assignment", "unsupported port assignment policy");
  const auto& enabledTiles = requiredArray(lsu, "enabled_tiles", "lsu");
  if (enabledTiles.size() > model.memory_.ports)
    fail("lsu.enabled_tiles", "enabled LSU count exceeds memory port count");
  std::set<std::pair<unsigned, unsigned>> seenTiles;
  std::set<unsigned> seenPorts;
  for (std::size_t index = 0; index < enabledTiles.size(); ++index) {
    const auto context = "lsu.enabled_tiles[" + std::to_string(index) + "]";
    if (!enabledTiles[index].is_object())
      fail(context, "must be an object");
    LsuTileDesc tile;
    tile.row = required<unsigned>(enabledTiles[index], "row", context);
    tile.col = required<unsigned>(enabledTiles[index], "col", context);
    tile.portId = required<unsigned>(enabledTiles[index], "port_id", context);
    if (tile.row >= model.array_.rows || tile.col >= model.array_.cols)
      fail(context, "tile coordinate is outside the array");
    if (tile.portId >= model.memory_.ports)
      fail(context + ".port_id", "is outside the memory port range");
    if (!seenTiles.emplace(tile.row, tile.col).second)
      fail(context, "duplicates an enabled LSU tile");
    if (!seenPorts.insert(tile.portId).second)
      fail(context + ".port_id", "duplicates a memory port assignment");
    model.lsuTiles_.push_back(tile);
  }
  std::ranges::sort(model.lsuTiles_, [](const LsuTileDesc& lhs, const LsuTileDesc& rhs) {
    return std::pair{lhs.row, lhs.col} < std::pair{rhs.row, rhs.col};
  });
  for (unsigned rank = 0; rank < model.lsuTiles_.size(); ++rank) {
    if (model.lsuTiles_[rank].portId != rank)
      fail("lsu.enabled_tiles", "port IDs must be dense row-major ranks");
  }

  const auto& params = requiredObject(root, "parameters", "");
  requireEqual(model.array_.rows, positiveUnsigned(params, "array_rows", "parameters"),
               "parameters.array_rows");
  requireEqual(model.array_.cols, positiveUnsigned(params, "array_cols", "parameters"),
               "parameters.array_cols");
  requireEqual(model.array_.dataWidth, positiveUnsigned(params, "data_width", "parameters"),
               "parameters.data_width");
  requireEqual(model.array_.predicateWidth, positiveUnsigned(params, "pred_width", "parameters"),
               "parameters.pred_width");
  requireEqual(model.dataRF_.depth, positiveUnsigned(params, "data_rf_depth", "parameters"),
               "parameters.data_rf_depth");
  requireEqual(model.predicateRF_.depth, positiveUnsigned(params, "pred_rf_depth", "parameters"),
               "parameters.pred_rf_depth");
  requireEqual(model.memory_.depth, positiveUnsigned(params, "scratchpad_depth", "parameters"),
               "parameters.scratchpad_depth");
  requireEqual(model.memory_.widthBits, positiveUnsigned(params, "data_width", "parameters"),
               "parameters.data_width/memory.width_bits");
  requireEqual(model.memory_.ports, positiveUnsigned(params, "shared_mem_ports", "parameters"),
               "parameters.shared_mem_ports");
  requireEqual(model.memory_.loadLatency, positiveUnsigned(params, "load_latency", "parameters"),
               "parameters.load_latency");
  requireEqual(model.interconnect_.hopLatency,
               positiveUnsigned(params, "mesh_hop_latency", "parameters"),
               "parameters.mesh_hop_latency");
  requireEqual(parsedLayout.rawWidth_,
               positiveUnsigned(params, "raw_control_word_width_bits", "parameters"),
               "parameters.raw_control_word_width_bits");
  requireEqual(parsedLayout.physicalWidth_,
               positiveUnsigned(params, "physical_control_word_width_bits", "parameters"),
               "parameters.physical_control_word_width_bits");
  requireEqual(parsedLayout.chunks_, positiveUnsigned(params, "control_word_chunks", "parameters"),
               "parameters.control_word_chunks");

  const auto& legacyControlSchema = requiredObject(root, "control_schema", "");
  if (required<std::string>(legacyControlSchema, "encoding", "control_schema") !=
      "lsb_first_fixed_field_order")
    fail("control_schema.encoding", "unsupported compatibility encoding");
  requireEqual(parsedLayout.chunkBits_,
               positiveUnsigned(legacyControlSchema, "alignment_bits", "control_schema"),
               "control_schema.alignment_bits");
  requireEqual(parsedLayout.rawWidth_,
               positiveUnsigned(legacyControlSchema, "raw_width_bits", "control_schema"),
               "control_schema.raw_width_bits");
  requireEqual(parsedLayout.physicalWidth_,
               positiveUnsigned(legacyControlSchema, "physical_width_bits", "control_schema"),
               "control_schema.physical_width_bits");
  requireEqual(parsedLayout.chunks_,
               positiveUnsigned(legacyControlSchema, "chunks", "control_schema"),
               "control_schema.chunks");

  return model;
}

bool TargetModel::tileHasLSU(unsigned row, unsigned col) const noexcept {
  return std::ranges::any_of(lsuTiles_, [row, col](const LsuTileDesc& tile) {
    return tile.row == row && tile.col == col;
  });
}

bool TargetModel::tileSupportsOperation(unsigned row, unsigned col,
                                        std::string_view operationName) const noexcept {
  if (row >= array_.rows || col >= array_.cols)
    return false;
  const auto* operation = findOperation(operationName);
  if (!operation)
    return false;
  if (operation->executionClass == TargetExecutionClass::LSU)
    return tileHasLSU(row, col);
  const auto overrideIt = tileOperationOverrides_.find(tileKey(row, col));
  const auto& supported =
      overrideIt == tileOperationOverrides_.end() ? defaultFuOperations_ : overrideIt->second;
  return supported.contains(operation->id);
}

std::vector<std::pair<unsigned, unsigned>>
TargetModel::compatibleTiles(std::string_view operationName) const {
  std::vector<std::pair<unsigned, unsigned>> result;
  for (unsigned row = 0; row < array_.rows; ++row)
    for (unsigned col = 0; col < array_.cols; ++col)
      if (tileSupportsOperation(row, col, operationName))
        result.emplace_back(row, col);
  return result;
}

unsigned TargetModel::memoryDependenceSeparation(ir::MemoryDepKind kind) const noexcept {
  switch (kind) {
  case ir::MemoryDepKind::RAW:
    return memory_.rawDependenceSeparation;
  case ir::MemoryDepKind::WAR:
    return memory_.warDependenceSeparation;
  case ir::MemoryDepKind::WAW:
    return memory_.wawDependenceSeparation;
  }
  return 0;
}

bool TargetModel::supportsValueType(const ir::ValueType& type) const noexcept {
  if (type.kind == ir::ValueKind::Void)
    return type.bitWidth == 0;
  if (type.kind == ir::ValueKind::Predicate)
    return type.bitWidth == array_.predicateWidth;
  if (isMappingResearchTarget())
    return std::ranges::find(supportedValueTypes_, type) != supportedValueTypes_.end();
  return type.kind == ir::ValueKind::Integer && type.bitWidth == array_.dataWidth;
}

const TargetOperationDesc* TargetModel::findOperation(std::string_view name) const noexcept {
  const auto it = operationIndices_.find(std::string(name));
  return it == operationIndices_.end() ? nullptr : &operations_[it->second];
}

const TargetOperationDesc& TargetModel::operation(std::string_view name) const {
  const auto* result = findOperation(name);
  if (!result)
    throw std::out_of_range("unknown target operation: " + std::string(name));
  return *result;
}

bool TargetModel::hasExecutionClass(TargetExecutionClass executionClass) const noexcept {
  return executionResourceCount(executionClass) != 0;
}

std::uint64_t
TargetModel::executionResourceCount(TargetExecutionClass executionClass) const noexcept {
  if (executionClass == TargetExecutionClass::LSU)
    return lsuTiles_.size();
  // The current contract describes a homogeneous scalar FU fabric. A future
  // heterogeneous target can replace this with per-tile capability metadata.
  return static_cast<std::uint64_t>(array_.rows) * array_.cols;
}

std::uint64_t
TargetModel::compatibleResourceCount(const TargetOperationDesc& operation) const noexcept {
  if (!isMappingResearchTarget() && !hasValidEncoding(operation))
    return 0;
  if (operation.executionClass == TargetExecutionClass::LSU)
    return lsuTiles_.size();
  std::uint64_t count = 0;
  for (unsigned row = 0; row < array_.rows; ++row)
    for (unsigned col = 0; col < array_.cols; ++col)
      if (tileSupportsOperation(row, col, operation.id))
        ++count;
  return count;
}

bool TargetModel::isOperationExecutable(std::string_view name) const noexcept {
  const auto* operationDesc = findOperation(name);
  return operationDesc != nullptr && isOperationExecutable(*operationDesc);
}

bool TargetModel::isOperationExecutable(const TargetOperationDesc& operationDesc) const noexcept {
  return compatibleResourceCount(operationDesc) != 0;
}

bool TargetModel::hasValidEncoding(const TargetOperationDesc& operation) const noexcept {
  const auto* registered = findOperation(operation.id);
  if (!registered || *registered != operation || !operation.encoding)
    return false;

  const auto domainIt = encodings_.find(operation.encoding->domain);
  return domainIt != encodings_.end() && domainIt->second.contains(operation.encoding->symbol);
}

unsigned TargetModel::encodingValue(std::string_view domain, std::string_view name) const {
  const auto domainIt = encodings_.find(std::string(domain));
  if (domainIt == encodings_.end())
    throw std::runtime_error("unknown target encoding domain: " + std::string(domain));
  const auto valueIt = domainIt->second.find(std::string(name));
  if (valueIt == domainIt->second.end())
    throw std::runtime_error("unknown target encoding " + std::string(domain) + "." +
                             std::string(name));
  return valueIt->second;
}

std::string TargetModel::encodingName(std::string_view domain, unsigned value) const {
  const auto domainIt = reverseEncodings_.find(std::string(domain));
  if (domainIt == reverseEncodings_.end())
    throw std::runtime_error("unknown target encoding domain: " + std::string(domain));
  const auto nameIt = domainIt->second.find(value);
  if (nameIt == domainIt->second.end())
    throw std::runtime_error("unknown numeric target encoding " + std::string(domain) + "=" +
                             std::to_string(value));
  return nameIt->second;
}

bool RegisterFileDesc::appliesToTile(unsigned row, unsigned col) const noexcept {
  return applicableTiles.empty() ||
         std::ranges::find(applicableTiles, std::pair{row, col}) != applicableTiles.end();
}

bool RegisterFileDesc::allocates(unsigned index) const noexcept {
  return std::ranges::find(allocatableIndices, index) != allocatableIndices.end();
}

const RegisterFileDesc* TargetModel::registerBank(RegisterBankDomain domain, unsigned row,
                                                  unsigned col) const noexcept {
  const auto& bank = domain == RegisterBankDomain::Data ? dataRF_ : predicateRF_;
  return bank.appliesToTile(row, col) ? &bank : nullptr;
}

} // namespace cgra
