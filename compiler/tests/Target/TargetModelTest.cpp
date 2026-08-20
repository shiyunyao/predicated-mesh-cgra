// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetModel.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
const std::filesystem::path RepositoryRoot = CGRA_REPOSITORY_ROOT;
const std::filesystem::path TargetPath = RepositoryRoot / "target/cgra_v2.json";
const std::filesystem::path ManifestPath =
    RepositoryRoot / "examples/schedules/shared_memory_cross_lsu_4x4.json";

int Failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++Failures;
  }
}

Json loadJson(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot open test JSON: " + path.string());
  Json value;
  stream >> value;
  return value;
}

class TemporaryTarget {
public:
  explicit TemporaryTarget(const Json &target) {
    static unsigned serial = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("cgra-target-contract-test-" + std::to_string(++serial) + ".json");
    std::ofstream stream(path_);
    stream << target.dump(2) << '\n';
  }

  ~TemporaryTarget() { std::filesystem::remove(path_); }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void expectRejected(Json target, const std::function<void(Json &)> &mutate,
                    std::string_view expected) {
  mutate(target);
  const TemporaryTarget file(target);
  try {
    static_cast<void>(cgra::TargetModel::loadFromFile(file.path()));
    check(false, "malformed target unexpectedly loaded: " + std::string(expected));
  } catch (const std::exception &error) {
    check(std::string(error.what()).find(expected) != std::string::npos,
          "malformed target diagnostic lacks context '" + std::string(expected) +
              "': " + error.what());
  }
}

cgra::EncodedControl parseChunks(const Json &chunks) {
  cgra::EncodedControl encoded;
  check(chunks.is_array() && chunks.size() == encoded.chunks.size(),
        "manifest control must contain four chunks");
  for (std::size_t index = 0; index < encoded.chunks.size(); ++index) {
    const auto text = chunks.at(index).get<std::string>();
    encoded.chunks[index] = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
  }
  return encoded;
}

void testCanonicalResources(const cgra::TargetModel &target) {
  check(target.name() == "cgra_v2_shared4p", "target name");
  check(target.contractVersion() == 2, "target contract version");
  check(target.array().rows == 4 && target.array().cols == 4, "array dimensions");
  check(target.array().dataWidth == 32 && target.array().predicateWidth == 1,
        "data and predicate widths");
  check(!target.array().hardwareBranch, "hardware branch disabled");
  check(target.dataRF().depth == 16 && target.dataRF().readPorts == 2 &&
            target.dataRF().writePorts == 2,
        "DataRF resources");
  check(target.dataRF().writePortSources.at("W0") ==
            std::vector<std::string>{"FU_DATA_RESULT"} &&
            target.dataRF().writePortSources.at("W1").size() == 9,
        "DataRF write-port source constraints");
  check(target.predicateRF().depth == 16 &&
            target.predicateRF().readPorts == 2 &&
            target.predicateRF().writePorts == 2,
        "PredicateRF resources");
  check(target.predicateRF().writePortSources.at("W0") ==
            std::vector<std::string>{"FU_PRED_RESULT"} &&
            target.predicateRF().writePortSources.at("W1").size() == 8,
        "PredicateRF write-port source constraints");
  check(target.dataNetwork().registeredLinks &&
            target.dataNetwork().hopLatency == 1 &&
            !target.dataNetwork().inputBuffering &&
            !target.dataNetwork().runtimeArbitration &&
            target.dataNetwork().compilerRouted &&
            target.dataNetwork().separateResourceDomain &&
            target.dataNetwork().channelsPerDirectionPerLink == 1,
        "network timing and arbitration");
  check(target.memory().model == "shared_multiport_scratchpad" &&
            target.memory().addressUnit == "word" &&
            target.memory().depth == 4096 && target.memory().ports == 4 &&
            target.memory().loadLatency == 2 &&
            target.memory().maxIssuePerLsuPerCycle == 1 &&
            target.memory().maxIssuePerPortPerCycle == 1 &&
            !target.memory().runtimeStall &&
            !target.memory().runtimeArbitration,
        "shared memory resources and timing");
  check(target.loopExecution().supported &&
            target.loopExecution().model == "finite_modulo_replay" &&
            !target.loopExecution().rotatingRegisters &&
            !target.loopExecution().loopCounterOperand &&
            !target.loopExecution().sameAddressRfReadWriteRecurrence,
        "loop execution restrictions");
  check(target.controlLayout().rawWidth() == 126 &&
            target.controlLayout().physicalWidth() == 128 &&
            target.controlLayout().chunks() == 4 &&
            target.controlLayout().chunkBits() == 32 &&
            target.controlLayout().paddingLsb() == 126 &&
            target.controlLayout().paddingWidth() == 2,
        "control dimensions and padding");
  check(target.lsuTiles().size() == 4, "four enabled LSU tiles");
  for (unsigned row = 0; row < 4; ++row) {
    check(target.tileHasLSU(row, 0), "left-column LSU enabled");
    check(!target.tileHasLSU(row, 1), "non-left-column LSU disabled");
    check(target.lsuTiles().at(row).portId == row,
          "row-major LSU port assignment");
  }
}

void testEncodingConsistency(const cgra::TargetModel &target) {
  const std::map<std::string, std::map<std::string, unsigned>> expected = {
      {"op", {{"NOP", 0}, {"PASS", 1}, {"ADD", 2}, {"SUB", 3},
              {"MUL", 4}, {"AND", 5}, {"OR", 6}, {"XOR", 7},
              {"SHL", 8}, {"LSHR", 9}, {"SELECT", 10},
              {"CMP_EQ", 16}, {"CMP_NE", 17}, {"CMP_ULT", 18},
              {"CMP_ULE", 19}, {"PPASS", 32}, {"PNOT", 33},
              {"PAND", 34}, {"POR", 35}}},
      {"data_source", {{"RF_A", 0}, {"RF_B", 1}, {"NORTH_DATA_IN", 2},
                       {"SOUTH_DATA_IN", 3}, {"EAST_DATA_IN", 4},
                       {"WEST_DATA_IN", 5}, {"CONST_DATA", 6},
                       {"LSU_LOAD_DATA", 7}, {"ZERO", 8}}},
      {"predicate_source", {{"RF_A", 0}, {"RF_B", 1},
                            {"NORTH_PRED_IN", 2}, {"SOUTH_PRED_IN", 3},
                            {"EAST_PRED_IN", 4}, {"WEST_PRED_IN", 5},
                            {"CONST_TRUE", 6}, {"CONST_FALSE", 7}}},
      {"route_data_source", {{"NONE", 0}, {"NORTH_DATA_IN", 1},
                             {"SOUTH_DATA_IN", 2}, {"EAST_DATA_IN", 3},
                             {"WEST_DATA_IN", 4}, {"FU_DATA_RESULT", 5},
                             {"RF_A", 6}, {"RF_B", 7}, {"CONST_DATA", 8},
                             {"LSU_LOAD_DATA", 9}, {"ZERO", 10}}},
      {"route_predicate_source", {{"NONE", 0}, {"NORTH_PRED_IN", 1},
                                  {"SOUTH_PRED_IN", 2}, {"EAST_PRED_IN", 3},
                                  {"WEST_PRED_IN", 4}, {"FU_PRED_RESULT", 5},
                                  {"RF_A", 6}, {"RF_B", 7},
                                  {"CONST_TRUE", 8}, {"CONST_FALSE", 9}}},
      {"lsu_op", {{"NONE", 0}, {"LOAD", 1}, {"STORE", 2},
                  {"RESERVED", 3}}},
  };

  for (const auto &[domain, entries] : expected) {
    for (const auto &[name, value] : entries) {
      check(target.encodingValue(domain, name) == value,
            "numeric encoding " + domain + "." + name);
      check(target.encodingName(domain, value) == name,
            "reverse numeric encoding " + domain + "." + name);
    }
  }
}

void testKnownControl(const cgra::TargetModel &target) {
  cgra::TileControl control;
  control.op = "ADD";
  control.srcA = "RF_A";
  control.srcB = "RF_B";
  control.srcP0 = "CONST_TRUE";
  control.srcP1 = "CONST_FALSE";
  control.dataRfReadAddrA = 3;
  control.dataRfReadAddrB = 4;
  control.predicateRfReadAddrA = 5;
  control.predicateRfReadAddrB = 6;
  control.dataWrite0Enable = true;
  control.dataWrite0Addr = 7;
  control.dataWrite1Enable = true;
  control.dataWrite1Addr = 8;
  control.dataWrite1Source = "LSU_LOAD_DATA";
  control.predicateWrite0Enable = true;
  control.predicateWrite0Addr = 9;
  control.predicateWrite1Enable = true;
  control.predicateWrite1Addr = 10;
  control.predicateWrite1Source = "WEST_PRED_IN";
  control.dataRoutes[0] = {true, "FU_DATA_RESULT"};
  control.dataRoutes[1] = {true, "RF_A"};
  control.dataRoutes[2] = {true, "CONST_DATA"};
  control.dataRoutes[3] = {true, "LSU_LOAD_DATA"};
  control.predicateRoutes[0] = {true, "FU_PRED_RESULT"};
  control.predicateRoutes[1] = {true, "RF_A"};
  control.predicateRoutes[2] = {true, "CONST_TRUE"};
  control.predicateRoutes[3] = {true, "CONST_FALSE"};
  control.constantAddr = 11;
  control.lsuOp = "STORE";
  control.lsuAddrSource = "RF_A";
  control.lsuStoreDataSource = "RF_B";
  control.lsuCommitPredicateEnable = true;
  control.lsuCommitPredicateInvert = true;
  control.lsuCommitPredicateSource = "RF_A";

  const auto encoded = cgra::encode(control, target);
  const std::array<std::uint32_t, 4> expected = {
      0x50dd8402U, 0x6b378bd9U, 0x6ae716adU, 0x0310ae71U};
  check(encoded.chunks == expected, "known semantic control encoding");
  check(cgra::encode(cgra::decode(encoded, target), target) == encoded,
        "known control decode/encode round-trip");
}

void testManifestRoundTrip(const cgra::TargetModel &target) {
  const auto manifest = loadJson(ManifestPath);
  unsigned count = 0;
  for (const auto &tile : manifest.at("program").at("tiles")) {
    for (const auto &entry : tile.at("control")) {
      const auto original = parseChunks(entry.at("chunks"));
      const auto roundTrip = cgra::encode(cgra::decode(original, target), target);
      check(roundTrip == original,
            "manifest control round-trip tile=(" +
                std::to_string(tile.at("row").get<unsigned>()) + "," +
                std::to_string(tile.at("col").get<unsigned>()) + ") pc=" +
                std::to_string(entry.at("pc").get<unsigned>()));
      ++count;
    }
  }
  check(count == 21, "all retained programmed controls were tested");
}

void testMalformedTargets(const Json &canonical) {
  expectRejected(canonical, [](Json &json) { json.erase("schema"); }, "schema");
  expectRejected(canonical,
                 [](Json &json) { json["target_contract_version"] = 3; },
                 "unsupported contract version");
  expectRejected(canonical, [](Json &json) { json["array"]["rows"] = 0; },
                 "array.rows");
  expectRejected(canonical,
                 [](Json &json) { json["data_rf"]["read_ports"] = 0; },
                 "data_rf.read_ports");
  expectRejected(canonical, [](Json &json) { json["memory"]["ports"] = 3; },
                 "enabled LSU count exceeds memory port count");
  expectRejected(canonical,
                 [](Json &json) { json["encodings"]["op"]["PASS"] = 0; },
                 "duplicate numeric value");
  expectRejected(canonical,
                 [](Json &json) { json["encodings"]["op"]["ADD"] = 64; },
                 "value does not fit field op");
  expectRejected(canonical,
                 [](Json &json) { json["encodings"].erase("data_source"); },
                 "missing encoding domain data_source");
  expectRejected(canonical,
                 [](Json &json) { json["control_layout"]["fields"][1]["lsb"] = 0; },
                 "overlaps another field");
  expectRejected(canonical,
                 [](Json &json) { json["control_layout"]["fields"].back()["width"] = 5; },
                 "extends beyond raw_width");
  expectRejected(canonical,
                 [](Json &json) { json["control_layout"]["physical_width"] = 125; },
                 "must be at least raw_width");
  expectRejected(canonical,
                 [](Json &json) { json["control_layout"]["chunks"] = 3; },
                 "exactly four 32-bit chunks");
  expectRejected(canonical,
                 [](Json &json) { json["control_layout"]["padding"]["value"] = 1; },
                 "only zero padding is supported");
  expectRejected(canonical,
                 [](Json &json) { json["lsu"]["enabled_tiles"][0]["row"] = 4; },
                 "outside the array");
  expectRejected(canonical,
                 [](Json &json) { json["lsu"]["port_assignment"] = "arbitrary"; },
                 "unsupported port assignment policy");
  expectRejected(canonical,
                 [](Json &json) {
                   json["lsu"]["enabled_tiles"][0]["port_id"] = 1;
                   json["lsu"]["enabled_tiles"][1]["port_id"] = 0;
                 },
                 "port IDs must be dense row-major ranks");
  expectRejected(canonical,
                 [](Json &json) { json["control_schema"]["chunks"] = 3; },
                 "control_schema.chunks");
  expectRejected(canonical,
                 [](Json &json) { json["lsu"]["enabled_tiles"][1] = json["lsu"]["enabled_tiles"][0]; },
                 "duplicates an enabled LSU tile");
}

void testPaddingRejected(const cgra::TargetModel &target) {
  cgra::EncodedControl encoded;
  encoded.chunks[3] = 0x80000000U;
  try {
    static_cast<void>(cgra::decode(encoded, target));
    check(false, "non-zero padding unexpectedly decoded");
  } catch (const std::exception &error) {
    check(std::string(error.what()).find("padding bits must be zero") !=
              std::string::npos,
          "padding diagnostic");
  }
}

} // namespace

int main() {
  try {
    const auto target = cgra::TargetModel::loadFromFile(TargetPath);
    testCanonicalResources(target);
    testEncodingConsistency(target);
    testKnownControl(target);
    testManifestRoundTrip(target);
    testMalformedTargets(loadJson(TargetPath));
    testPaddingRejected(target);
  } catch (const std::exception &error) {
    std::cerr << "UNCAUGHT TEST ERROR: " << error.what() << '\n';
    return 1;
  }

  if (Failures != 0) {
    std::cerr << Failures << " target contract test(s) failed\n";
    return 1;
  }
  std::cout << "CGRA_TARGET_CONTRACT_TEST_PASS\n";
  return 0;
}
