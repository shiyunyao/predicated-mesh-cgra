// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetModel.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace cgra {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string &context, const std::string &message) {
  throw std::runtime_error("target." + context + ": " + message);
}

const Json &requiredObject(const Json &parent, const std::string &key,
                           const std::string &context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required object");
  const auto &value = parent.at(key);
  if (!value.is_object())
    fail(context + "." + key, "must be an object");
  return value;
}

const Json &requiredArray(const Json &parent, const std::string &key,
                          const std::string &context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required array");
  const auto &value = parent.at(key);
  if (!value.is_array())
    fail(context + "." + key, "must be an array");
  return value;
}

template <typename T>
T required(const Json &parent, const std::string &key,
           const std::string &context) {
  if (!parent.contains(key))
    fail(context + "." + key, "missing required value");
  try {
    return parent.at(key).get<T>();
  } catch (const Json::exception &error) {
    fail(context + "." + key, std::string("has invalid type: ") + error.what());
  }
}

unsigned positiveUnsigned(const Json &parent, const std::string &key,
                          const std::string &context) {
  const auto value = required<std::uint64_t>(parent, key, context);
  if (value == 0)
    fail(context + "." + key, "must be greater than zero");
  if (value > std::numeric_limits<unsigned>::max())
    fail(context + "." + key, "does not fit unsigned");
  return static_cast<unsigned>(value);
}

RegisterFileDesc parseRegisterFile(const Json &root, const std::string &key) {
  const auto &json = requiredObject(root, key, "");
  RegisterFileDesc desc;
  desc.depth = positiveUnsigned(json, "depth", key);
  desc.readPorts = positiveUnsigned(json, "read_ports", key);
  desc.writePorts = positiveUnsigned(json, "write_ports", key);
  desc.sameCycleReadWriteSameAddress =
      required<std::string>(json, "same_cycle_read_write_same_address", key);
  desc.sameCycleMultiwriteSameAddress =
      required<std::string>(json, "same_cycle_multiwrite_same_address", key);
  if (desc.sameCycleReadWriteSameAddress != "illegal")
    fail(key + ".same_cycle_read_write_same_address", "unsupported policy");
  if (desc.sameCycleMultiwriteSameAddress != "illegal")
    fail(key + ".same_cycle_multiwrite_same_address", "unsupported policy");
  const auto &ports = requiredObject(json, "write_ports_detail", key);
  for (const auto &[port, sources] : ports.items()) {
    if (!sources.is_object() || !sources.contains("sources") ||
        !sources.at("sources").is_array() || sources.at("sources").empty())
      fail(key + ".write_ports_detail." + port + ".sources",
           "must be a non-empty array");
    auto &portSources = desc.writePortSources[port];
    for (const auto &source : sources.at("sources")) {
      if (!source.is_string())
        fail(key + ".write_ports_detail." + port + ".sources",
             "entries must be strings");
      portSources.push_back(source.get<std::string>());
    }
  }
  if (desc.writePortSources.size() != desc.writePorts)
    fail(key + ".write_ports_detail",
         "must describe every register-file write port exactly once");
  return desc;
}

void requireEqual(unsigned lhs, unsigned rhs, const std::string &context) {
  if (lhs != rhs)
    fail(context, "inconsistent duplicate contract values");
}

void parseNetworkDomain(const Json &network, const std::string &key,
                        InterconnectDesc &desc) {
  const auto &domain = requiredObject(network, key, "interconnect");
  desc.separateResourceDomain =
      required<bool>(domain, "separate_resource_domain", "interconnect." + key);
  desc.channelsPerDirectionPerLink = positiveUnsigned(
      domain, "channels_per_direction_per_link", "interconnect." + key);
  if (!desc.separateResourceDomain)
    fail("interconnect." + key + ".separate_resource_domain",
         "must be enabled for the data and predicate networks");
  if (desc.channelsPerDirectionPerLink != 1)
    fail("interconnect." + key + ".channels_per_direction_per_link",
         "only one compiler-routed channel is supported");
}

} // namespace

TargetModel TargetModel::loadFromFile(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot open target contract: " + path.string());

  Json root;
  try {
    stream >> root;
  } catch (const Json::exception &error) {
    throw std::runtime_error("cannot parse target contract " + path.string() +
                             ": " + error.what());
  }
  if (!root.is_object())
    fail("", "root must be an object");

  TargetModel model;
  const auto schema = required<std::string>(root, "schema", "");
  if (schema != "cgra.target.v2")
    fail("schema", "unsupported schema " + schema);
  model.contractVersion_ =
      positiveUnsigned(root, "target_contract_version", "");
  if (model.contractVersion_ != 2)
    fail("target_contract_version", "unsupported contract version " +
                                         std::to_string(model.contractVersion_));
  model.name_ = required<std::string>(root, "name", "");
  if (model.name_.empty())
    fail("name", "must not be empty");

  const auto &array = requiredObject(root, "array", "");
  model.array_.rows = positiveUnsigned(array, "rows", "array");
  model.array_.cols = positiveUnsigned(array, "cols", "array");
  model.array_.dataWidth = positiveUnsigned(array, "data_width", "array");
  model.array_.predicateWidth =
      positiveUnsigned(array, "predicate_width", "array");
  model.array_.hardwareBranch =
      required<bool>(array, "hardware_branch", "array");

  model.dataRF_ = parseRegisterFile(root, "data_rf");
  model.predicateRF_ = parseRegisterFile(root, "predicate_rf");

  const auto &network = requiredObject(root, "interconnect", "");
  model.interconnect_.topology =
      required<std::string>(network, "topology", "interconnect");
  model.interconnect_.registeredLinks =
      required<bool>(network, "registered_links", "interconnect");
  model.interconnect_.hopLatency =
      positiveUnsigned(network, "hop_latency", "interconnect");
  model.interconnect_.inputBuffering =
      required<bool>(network, "input_buffering", "interconnect");
  model.interconnect_.runtimeArbitration =
      required<bool>(network, "runtime_arbitration", "interconnect");
  model.interconnect_.compilerRouted =
      required<bool>(network, "compiler_routed", "interconnect");
  if (model.interconnect_.topology != "mesh_2d")
    fail("interconnect.topology", "unsupported topology");
  if (!model.interconnect_.registeredLinks ||
      model.interconnect_.inputBuffering ||
      model.interconnect_.runtimeArbitration ||
      !model.interconnect_.compilerRouted)
    fail("interconnect", "does not describe the compiler-controlled registered mesh");
  parseNetworkDomain(network, "data_network", model.interconnect_);
  InterconnectDesc predicateNetwork = model.interconnect_;
  parseNetworkDomain(network, "predicate_network", predicateNetwork);
  if (predicateNetwork.separateResourceDomain !=
          model.interconnect_.separateResourceDomain ||
      predicateNetwork.channelsPerDirectionPerLink !=
          model.interconnect_.channelsPerDirectionPerLink)
    fail("interconnect", "data and predicate network resources disagree");

  const auto &memory = requiredObject(root, "memory", "");
  model.memory_.model = required<std::string>(memory, "model", "memory");
  model.memory_.addressUnit =
      required<std::string>(memory, "address_unit", "memory");
  model.memory_.depth = positiveUnsigned(memory, "depth", "memory");
  model.memory_.widthBits = positiveUnsigned(memory, "width_bits", "memory");
  model.memory_.ports = positiveUnsigned(memory, "ports", "memory");
  model.memory_.loadLatency =
      positiveUnsigned(memory, "load_latency", "memory");
  model.memory_.maxIssuePerLsuPerCycle =
      positiveUnsigned(memory, "max_issue_per_lsu_per_cycle", "memory");
  model.memory_.maxIssuePerPortPerCycle =
      positiveUnsigned(memory, "max_issue_per_port_per_cycle", "memory");
  model.memory_.sameAddressPolicy =
      required<std::string>(memory, "same_address_policy", "memory");
  model.memory_.runtimeStall =
      required<bool>(memory, "runtime_stall", "memory");
  model.memory_.runtimeArbitration =
      required<bool>(memory, "runtime_arbitration", "memory");
  if (model.memory_.model != "shared_multiport_scratchpad")
    fail("memory.model", "unsupported memory model");
  if (model.memory_.addressUnit != "word")
    fail("memory.address_unit", "must be word");
  if (model.memory_.sameAddressPolicy != "multi_load_only")
    fail("memory.same_address_policy", "unsupported policy");
  if (model.memory_.runtimeStall || model.memory_.runtimeArbitration)
    fail("memory", "runtime stall/arbitration must be disabled");

  const auto &loop = requiredObject(root, "loop_execution", "");
  model.loopExecution_.supported =
      required<bool>(loop, "supported", "loop_execution");
  model.loopExecution_.model =
      required<std::string>(loop, "model", "loop_execution");
  model.loopExecution_.controlPhases =
      required<std::vector<std::string>>(loop, "control_phases", "loop_execution");
  model.loopExecution_.rotatingRegisters =
      required<bool>(loop, "rotating_registers", "loop_execution");
  model.loopExecution_.loopCounterOperand =
      required<bool>(loop, "loop_counter_operand", "loop_execution");
  model.loopExecution_.sameAddressRfReadWriteRecurrence = required<bool>(
      loop, "same_address_rf_read_write_recurrence", "loop_execution");
  if (!model.loopExecution_.supported ||
      model.loopExecution_.model != "finite_modulo_replay")
    fail("loop_execution", "unsupported loop execution model");
  if (model.loopExecution_.controlPhases !=
      std::vector<std::string>{"prologue", "kernel", "epilogue"})
    fail("loop_execution.control_phases", "must be prologue, kernel, epilogue");
  if (model.loopExecution_.rotatingRegisters ||
      model.loopExecution_.loopCounterOperand ||
      model.loopExecution_.sameAddressRfReadWriteRecurrence)
    fail("loop_execution", "unsupported loop resources are enabled");

  const auto &encodings = requiredObject(root, "encodings", "");
  for (const auto &[domain, entries] : encodings.items()) {
    if (!entries.is_object() || entries.empty())
      fail("encodings." + domain, "must be a non-empty object");
    std::unordered_set<unsigned> seenValues;
    for (const auto &[name, jsonValue] : entries.items()) {
      if (!jsonValue.is_number_unsigned() && !jsonValue.is_number_integer())
        fail("encodings." + domain + "." + name, "must be an unsigned integer");
      const auto signedValue = jsonValue.get<std::int64_t>();
      if (signedValue < 0 ||
          static_cast<std::uint64_t>(signedValue) >
              std::numeric_limits<unsigned>::max())
        fail("encodings." + domain + "." + name, "does not fit unsigned");
      const auto value = static_cast<unsigned>(signedValue);
      if (!seenValues.insert(value).second)
        fail("encodings." + domain, "duplicate numeric value " +
                                         std::to_string(value));
      model.encodings_[domain].emplace(name, value);
      model.reverseEncodings_[domain].emplace(value, name);
    }
  }

  const auto &layout = requiredObject(root, "control_layout", "");
  auto &parsedLayout = model.controlLayout_;
  parsedLayout.rawWidth_ = positiveUnsigned(layout, "raw_width", "control_layout");
  parsedLayout.physicalWidth_ =
      positiveUnsigned(layout, "physical_width", "control_layout");
  parsedLayout.chunks_ = positiveUnsigned(layout, "chunks", "control_layout");
  parsedLayout.chunkBits_ =
      positiveUnsigned(layout, "chunk_bits", "control_layout");
  parsedLayout.chunkOrder_ =
      required<std::string>(layout, "chunk_order", "control_layout");
  if (parsedLayout.physicalWidth_ < parsedLayout.rawWidth_)
    fail("control_layout.physical_width", "must be at least raw_width");
  if (parsedLayout.chunks_ != 4 || parsedLayout.chunkBits_ != 32)
    fail("control_layout", "EncodedControl requires exactly four 32-bit chunks");
  if (parsedLayout.chunks_ * parsedLayout.chunkBits_ !=
      parsedLayout.physicalWidth_)
    fail("control_layout", "chunks * chunk_bits must equal physical_width");
  if (parsedLayout.chunkOrder_ != "little_word")
    fail("control_layout.chunk_order", "unsupported chunk order");

  const auto &padding = requiredObject(layout, "padding", "control_layout");
  parsedLayout.paddingLsb_ =
      required<unsigned>(padding, "lsb", "control_layout.padding");
  parsedLayout.paddingWidth_ =
      required<unsigned>(padding, "width", "control_layout.padding");
  parsedLayout.paddingValue_ =
      required<std::uint64_t>(padding, "value", "control_layout.padding");
  if (parsedLayout.paddingLsb_ != parsedLayout.rawWidth_ ||
      parsedLayout.paddingWidth_ !=
          parsedLayout.physicalWidth_ - parsedLayout.rawWidth_)
    fail("control_layout.padding", "must cover exactly physical bits above raw_width");
  if (parsedLayout.paddingValue_ != 0)
    fail("control_layout.padding.value", "only zero padding is supported");

  std::vector<bool> covered(parsedLayout.rawWidth_, false);
  const auto &fields = requiredArray(layout, "fields", "control_layout");
  if (fields.empty())
    fail("control_layout.fields", "must not be empty");
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto &fieldJson = fields[index];
    const auto context = "control_layout.fields[" + std::to_string(index) + "]";
    if (!fieldJson.is_object())
      fail(context, "must be an object");
    ControlField field;
    field.name = required<std::string>(fieldJson, "name", context);
    field.lsb = required<unsigned>(fieldJson, "lsb", context);
    field.width = positiveUnsigned(fieldJson, "width", context);
    if (field.name.empty())
      fail(context + ".name", "must not be empty");
    if (field.lsb > parsedLayout.rawWidth_ ||
        field.width > parsedLayout.rawWidth_ - field.lsb)
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
        fail(context + ".encoding", "references missing encoding domain " +
                                        *field.encoding);
      const std::uint64_t limit =
          field.width >= 64 ? std::numeric_limits<std::uint64_t>::max()
                            : (std::uint64_t{1} << field.width) - 1;
      for (const auto &[name, value] : domain->second) {
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

  const auto &lsu = requiredObject(root, "lsu", "");
  const auto portAssignment =
      required<std::string>(lsu, "port_assignment", "lsu");
  if (portAssignment != "enabled_tile_row_major_rank")
    fail("lsu.port_assignment", "unsupported port assignment policy");
  const auto &enabledTiles = requiredArray(lsu, "enabled_tiles", "lsu");
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
  std::ranges::sort(model.lsuTiles_, [](const LsuTileDesc &lhs,
                                       const LsuTileDesc &rhs) {
    return std::pair{lhs.row, lhs.col} < std::pair{rhs.row, rhs.col};
  });
  for (unsigned rank = 0; rank < model.lsuTiles_.size(); ++rank) {
    if (model.lsuTiles_[rank].portId != rank)
      fail("lsu.enabled_tiles", "port IDs must be dense row-major ranks");
  }

  const auto &params = requiredObject(root, "parameters", "");
  requireEqual(model.array_.rows, positiveUnsigned(params, "array_rows", "parameters"),
               "parameters.array_rows");
  requireEqual(model.array_.cols, positiveUnsigned(params, "array_cols", "parameters"),
               "parameters.array_cols");
  requireEqual(model.array_.dataWidth,
               positiveUnsigned(params, "data_width", "parameters"),
               "parameters.data_width");
  requireEqual(model.array_.predicateWidth,
               positiveUnsigned(params, "pred_width", "parameters"),
               "parameters.pred_width");
  requireEqual(model.dataRF_.depth,
               positiveUnsigned(params, "data_rf_depth", "parameters"),
               "parameters.data_rf_depth");
  requireEqual(model.predicateRF_.depth,
               positiveUnsigned(params, "pred_rf_depth", "parameters"),
               "parameters.pred_rf_depth");
  requireEqual(model.memory_.depth,
               positiveUnsigned(params, "scratchpad_depth", "parameters"),
               "parameters.scratchpad_depth");
  requireEqual(model.memory_.widthBits,
               positiveUnsigned(params, "data_width", "parameters"),
               "parameters.data_width/memory.width_bits");
  requireEqual(model.memory_.ports,
               positiveUnsigned(params, "shared_mem_ports", "parameters"),
               "parameters.shared_mem_ports");
  requireEqual(model.memory_.loadLatency,
               positiveUnsigned(params, "load_latency", "parameters"),
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
  requireEqual(parsedLayout.chunks_,
               positiveUnsigned(params, "control_word_chunks", "parameters"),
               "parameters.control_word_chunks");

  const auto &legacyControlSchema =
      requiredObject(root, "control_schema", "");
  if (required<std::string>(legacyControlSchema, "encoding", "control_schema") !=
      "lsb_first_fixed_field_order")
    fail("control_schema.encoding", "unsupported compatibility encoding");
  requireEqual(parsedLayout.chunkBits_,
               positiveUnsigned(legacyControlSchema, "alignment_bits",
                                "control_schema"),
               "control_schema.alignment_bits");
  requireEqual(parsedLayout.rawWidth_,
               positiveUnsigned(legacyControlSchema, "raw_width_bits",
                                "control_schema"),
               "control_schema.raw_width_bits");
  requireEqual(parsedLayout.physicalWidth_,
               positiveUnsigned(legacyControlSchema, "physical_width_bits",
                                "control_schema"),
               "control_schema.physical_width_bits");
  requireEqual(parsedLayout.chunks_,
               positiveUnsigned(legacyControlSchema, "chunks", "control_schema"),
               "control_schema.chunks");

  return model;
}

bool TargetModel::tileHasLSU(unsigned row, unsigned col) const noexcept {
  return std::ranges::any_of(lsuTiles_, [row, col](const LsuTileDesc &tile) {
    return tile.row == row && tile.col == col;
  });
}

unsigned TargetModel::encodingValue(std::string_view domain,
                                    std::string_view name) const {
  const auto domainIt = encodings_.find(std::string(domain));
  if (domainIt == encodings_.end())
    throw std::runtime_error("unknown target encoding domain: " +
                             std::string(domain));
  const auto valueIt = domainIt->second.find(std::string(name));
  if (valueIt == domainIt->second.end())
    throw std::runtime_error("unknown target encoding " + std::string(domain) +
                             "." + std::string(name));
  return valueIt->second;
}

std::string TargetModel::encodingName(std::string_view domain,
                                      unsigned value) const {
  const auto domainIt = reverseEncodings_.find(std::string(domain));
  if (domainIt == reverseEncodings_.end())
    throw std::runtime_error("unknown target encoding domain: " +
                             std::string(domain));
  const auto nameIt = domainIt->second.find(value);
  if (nameIt == domainIt->second.end())
    throw std::runtime_error("unknown numeric target encoding " +
                             std::string(domain) + "=" +
                             std::to_string(value));
  return nameIt->second;
}

} // namespace cgra
