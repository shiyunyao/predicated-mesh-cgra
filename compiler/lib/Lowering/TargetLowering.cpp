// SPDX-License-Identifier: MIT
#include "cgra/Lowering/TargetLowering.h"
#include "cgra/Lowering/ProgramManifest.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/RegisterAllocation/RFAllocationVerifier.h"
#include "cgra/Schedule/MaterializedScheduleVerifier.h"
#include "cgra/Target/TargetDFGVerifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace cgra::lowering {
namespace {

using Json = nlohmann::json;
using namespace cgra::mapping;
using namespace cgra::target;
using namespace cgra::schedule;
using namespace cgra::register_allocation;

struct LoweringError : std::runtime_error {
  TargetLoweringStatus status;
  LoweringError(TargetLoweringStatus status, std::string message)
      : std::runtime_error(std::move(message)), status(status) {}
};

std::size_t tileIndex(TileCoord tile, const TargetModel& target) {
  if (tile.row >= target.array().rows || tile.col >= target.array().cols)
    throw LoweringError(TargetLoweringStatus::SourceConnectivityViolation,
                        "tile is outside target array");
  return static_cast<std::size_t>(tile.row) * target.array().cols + tile.col;
}

unsigned directionIndex(Direction direction) {
  switch (direction) {
  case Direction::North:
    return 0;
  case Direction::South:
    return 1;
  case Direction::East:
    return 2;
  case Direction::West:
    return 3;
  }
  return 0;
}

Direction incomingDirection(Direction outgoing) { return opposite(outgoing); }

std::string dataInput(Direction direction) {
  switch (incomingDirection(direction)) {
  case Direction::North:
    return "NORTH_DATA_IN";
  case Direction::South:
    return "SOUTH_DATA_IN";
  case Direction::East:
    return "EAST_DATA_IN";
  case Direction::West:
    return "WEST_DATA_IN";
  }
  return "ZERO";
}

std::string predicateInput(Direction direction) {
  switch (incomingDirection(direction)) {
  case Direction::North:
    return "NORTH_PRED_IN";
  case Direction::South:
    return "SOUTH_PRED_IN";
  case Direction::East:
    return "EAST_PRED_IN";
  case Direction::West:
    return "WEST_PRED_IN";
  }
  return "CONST_FALSE";
}

std::string rfSource(RegisterBankDomain domain, bool second) {
  if (domain == RegisterBankDomain::Data)
    return second ? "RF_B" : "RF_A";
  return second ? "RF_B" : "RF_A";
}

std::string resultSource(TargetResultSource source) {
  switch (source) {
  case TargetResultSource::FuDataResult:
    return "FU_DATA_RESULT";
  case TargetResultSource::FuPredicateResult:
    return "FU_PRED_RESULT";
  case TargetResultSource::LsuLoadData:
    return "LSU_LOAD_DATA";
  case TargetResultSource::None:
    break;
  }
  throw LoweringError(TargetLoweringStatus::UnsupportedOperationLowering,
                      "operation has no materializable result source");
}

bool hasDataDomain(const TargetEdge& edge) { return edge.kind() == ir::Edge::Kind::Data; }

bool hasPredicateDomain(const TargetEdge& edge) { return edge.kind() == ir::Edge::Kind::Predicate; }

const TargetEdge* edgeForOperand(const TargetDFG& dfg, TargetNodeId node, unsigned operand) {
  for (auto edgeId : dfg.incoming(node)) {
    const auto& edge = dfg.edge(edgeId);
    if (hasDataDomain(edge)) {
      if (std::get<ir::DataEdgeInfo>(edge.info).dstOperand == operand)
        return &edge;
    } else if (hasPredicateDomain(edge) &&
               std::get<ir::PredicateEdgeInfo>(edge.info).dstOperand == operand) {
      return &edge;
    }
  }
  return nullptr;
}

const TargetOperandBinding* bindingForOperand(const TargetDFG& dfg, TargetNodeId node,
                                              unsigned operand) {
  for (const auto& binding : dfg.operandBindings())
    if (binding.node == node && binding.operand == operand)
      return &binding;
  return nullptr;
}

const ir::ConstantValue* constantFor(const TargetDFG& dfg, ir::ConstantId id) {
  for (const auto& constant : dfg.constants())
    if (constant.id == id)
      return &constant;
  return nullptr;
}

const StorageSegment* segmentForEdgeAtTile(const RFAllocatedMapping& mapping, TargetEdgeId edge,
                                           TileCoord tile) {
  for (const auto& segment : mapping.storageRequirements().segments())
    if (segment.edge == edge && segment.tile == tile)
      return &segment;
  return nullptr;
}

const MappedDependence& mappedEdge(const RFAllocatedMapping& mapping, TargetEdgeId edge) {
  return mapping.staged().modulo().dependence(edge);
}

struct TileBuilder {
  TileControl control;
  std::set<std::string> owned;
  TargetLoweringStats* stats = nullptr;
  const TargetModel* target = nullptr;
  TileCoord tile;

  void merge(const std::string& field, const std::string& value) {
    auto assign = [&](std::string& target) {
      if (!owned.insert(field).second) {
        if (target != value)
          throw LoweringError(TargetLoweringStatus::ControlFieldConflict,
                              "conflicting assignment to " + field);
        if (stats)
          ++stats->idempotentMerges;
        return;
      }
      target = value;
    };
    if (field == "op")
      assign(control.op);
    else if (field == "srcA")
      assign(control.srcA);
    else if (field == "srcB")
      assign(control.srcB);
    else if (field == "srcP0")
      assign(control.srcP0);
    else if (field == "srcP1")
      assign(control.srcP1);
    else if (field == "lsuOp")
      assign(control.lsuOp);
    else if (field == "lsuAddrSource")
      assign(control.lsuAddrSource);
    else if (field == "lsuStoreDataSource")
      assign(control.lsuStoreDataSource);
    else if (field == "lsuCommitPredicateSource")
      assign(control.lsuCommitPredicateSource);
    else if (field == "dataWrite1Source")
      assign(control.dataWrite1Source);
    else if (field == "predicateWrite1Source")
      assign(control.predicateWrite1Source);
  }

  void mergeBool(const std::string& field, bool value, bool& target) {
    if (!owned.insert(field).second) {
      if (target != value)
        throw LoweringError(TargetLoweringStatus::ControlFieldConflict,
                            "conflicting assignment to " + field);
      if (stats)
        ++stats->idempotentMerges;
      return;
    }
    target = value;
  }

  void mergeUnsigned(const std::string& field, unsigned value, unsigned& target) {
    if (!owned.insert(field).second) {
      if (target != value)
        throw LoweringError(TargetLoweringStatus::ControlFieldConflict,
                            "conflicting assignment to " + field);
      if (stats)
        ++stats->idempotentMerges;
      return;
    }
    target = value;
  }

  void source(unsigned sink, const std::string& value) {
    switch (static_cast<TargetControlSink>(sink)) {
    case TargetControlSink::FuDataA:
      merge("srcA", value);
      break;
    case TargetControlSink::FuDataB:
      merge("srcB", value);
      break;
    case TargetControlSink::FuPredicate0:
      merge("srcP0", value);
      break;
    case TargetControlSink::FuPredicate1:
      merge("srcP1", value);
      break;
    case TargetControlSink::LsuAddress:
      merge("lsuAddrSource", value);
      break;
    case TargetControlSink::LsuStoreData:
      merge("lsuStoreDataSource", value);
      break;
    case TargetControlSink::LsuCommitPredicate:
      merge("lsuCommitPredicateSource", value);
      mergeBool("lsuCommitPredicateEnable", true, control.lsuCommitPredicateEnable);
      break;
    }
  }

  void route(NetworkDomain domain, Direction direction, const std::string& value) {
    auto& route = domain == NetworkDomain::Data
                      ? control.dataRoutes[directionIndex(direction)]
                      : control.predicateRoutes[directionIndex(direction)];
    const auto prefix = domain == NetworkDomain::Data ? "dataRoute" : "predicateRoute";
    mergeBool(std::string(prefix) + std::to_string(directionIndex(direction)), true, route.enabled);
    const auto field = std::string(prefix) + std::to_string(directionIndex(direction)) + "Source";
    if (!owned.insert(field).second) {
      if (route.source != value)
        throw LoweringError(TargetLoweringStatus::ControlFieldConflict, "conflicting route source");
      if (stats)
        ++stats->idempotentMerges;
    } else {
      route.source = value;
    }
  }

  void dataWrite(unsigned port, unsigned address, const std::string& value) {
    if (target) {
      const auto* bank = target->registerBank(RegisterBankDomain::Data, tile.row, tile.col);
      if (!bank)
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "data RF bank is unavailable on tile");
      const auto it = bank->writePortSources.find("W" + std::to_string(port));
      if (it == bank->writePortSources.end() ||
          std::find(it->second.begin(), it->second.end(), value) == it->second.end())
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "data RF write source is not accepted by selected port");
    }
    if (port == 0) {
      mergeBool("dataWrite0Enable", true, control.dataWrite0Enable);
      mergeUnsigned("dataWrite0Addr", address, control.dataWrite0Addr);
      if (value != "FU_DATA_RESULT")
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "data W0 only accepts FU_DATA_RESULT");
    } else {
      mergeBool("dataWrite1Enable", true, control.dataWrite1Enable);
      mergeUnsigned("dataWrite1Addr", address, control.dataWrite1Addr);
      merge("dataWrite1Source", value);
    }
  }

  void predicateWrite(unsigned port, unsigned address, const std::string& value) {
    if (target) {
      const auto* bank = target->registerBank(RegisterBankDomain::Predicate, tile.row, tile.col);
      if (!bank)
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "predicate RF bank is unavailable on tile");
      const auto it = bank->writePortSources.find("W" + std::to_string(port));
      if (it == bank->writePortSources.end() ||
          std::find(it->second.begin(), it->second.end(), value) == it->second.end())
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "predicate RF write source is not accepted by selected port");
    }
    if (port == 0) {
      mergeBool("predicateWrite0Enable", true, control.predicateWrite0Enable);
      mergeUnsigned("predicateWrite0Addr", address, control.predicateWrite0Addr);
      if (value != "FU_PRED_RESULT")
        throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                            "predicate W0 only accepts FU_PRED_RESULT");
    } else {
      mergeBool("predicateWrite1Enable", true, control.predicateWrite1Enable);
      mergeUnsigned("predicateWrite1Addr", address, control.predicateWrite1Addr);
      merge("predicateWrite1Source", value);
    }
  }

  void constant(unsigned address) { mergeUnsigned("constantAddr", address, control.constantAddr); }
};

struct Lowerer {
  const TargetDFG& dfg;
  const TargetModel& target;
  const RFAllocatedMapping& mapping;
  const MaterializedSchedule& schedule;
  TargetLoweringOptions options;
  TargetLoweringStats stats;

  void event(const MaterializedEvent& event, TileBuilder& builder, std::uint32_t cycle) {
    (void)cycle;
    if (event.kind == MaterializedEventKind::BoundaryValueInject)
      throw LoweringError(TargetLoweringStatus::UnsupportedBoundaryProvider,
                          "boundary value provider is not defined by this target");
    if (event.kind == MaterializedEventKind::LiveOutBoundaryUse)
      return;
    if (!event.tile && event.kind != MaterializedEventKind::NodeIssue)
      throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                          "materialized event has no tile");
    if (event.kind == MaterializedEventKind::NodeIssue) {
      if (!event.node)
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                            "node issue has no node provenance");
      const auto& node = dfg.node(*event.node);
      const auto& op = target.operation(node.operation);
      if (!op.encoding)
        throw LoweringError(TargetLoweringStatus::UnsupportedOperationLowering,
                            "operation has no encoding binding");
      if (op.executionClass == TargetExecutionClass::FU)
        builder.merge("op", op.encoding->symbol);
      else
        builder.merge("lsuOp", op.encoding->symbol);
      ++stats.nodeIssues;
      for (const auto& [index, sink] : op.operandSinks) {
        const auto* edge = edgeForOperand(dfg, node.id, index);
        const auto* binding = bindingForOperand(dfg, node.id, index);
        std::string source;
        if (binding) {
          if (std::holds_alternative<ir::ExternalValueRef>(binding->source))
            throw LoweringError(TargetLoweringStatus::UnsupportedExternalProvider,
                                "external operand provider is not lowered in T012");
          const auto constantId = std::get<ir::ConstantRef>(binding->source).value;
          const auto* constant = constantFor(dfg, constantId);
          if (!constant)
            throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                                "constant operand references unknown constant");
          source =
              (sink == TargetControlSink::FuPredicate0 || sink == TargetControlSink::FuPredicate1 ||
               sink == TargetControlSink::LsuCommitPredicate)
                  ? (constant->bits != 0 ? "CONST_TRUE" : "CONST_FALSE")
                  : "CONST_DATA";
          builder.constant(options.constantImage.address(constantId));
        } else if (edge) {
          if (edge->kind() == ir::Edge::Kind::Memory)
            throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                                "memory dependence cannot provide an operand value");
          const auto* segment = segmentForEdgeAtTile(mapping, edge->id, *event.tile);
          if (segment) {
            const auto& allocation = mapping.allocationFor(segment->id);
            const auto reg = allocation.reg;
            const auto* bank = target.registerBank(segment->domain, reg.tile.row, reg.tile.col);
            if (!bank || allocation.readPort >= bank->readPorts)
              throw LoweringError(TargetLoweringStatus::RFAccessPortViolation,
                                  "RF read operand exceeds target read-port contract");
            source = rfSource(segment->domain, allocation.readPort == 1);
            builder.mergeUnsigned(
                segment->domain == RegisterBankDomain::Data
                    ? (allocation.readPort == 0 ? "dataRfReadAddrA" : "dataRfReadAddrB")
                    : (allocation.readPort == 0 ? "predicateRfReadAddrA" : "predicateRfReadAddrB"),
                reg.index,
                segment->domain == RegisterBankDomain::Data
                    ? (allocation.readPort == 0 ? builder.control.dataRfReadAddrA
                                                : builder.control.dataRfReadAddrB)
                    : (allocation.readPort == 0 ? builder.control.predicateRfReadAddrA
                                                : builder.control.predicateRfReadAddrB));
          } else {
            const auto& dep = mappedEdge(mapping, edge->id);
            if (!dep.transport || dep.transport->actions.empty())
              throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                                  "value operand has no transport or storage source");
            const auto& action = dep.transport->actions.back();
            if (!std::holds_alternative<LinkStep>(action))
              throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                                  "value operand storage provenance is incomplete");
            const auto& link = std::get<LinkStep>(action);
            source = edge->kind() == ir::Edge::Kind::Predicate ? predicateInput(link.direction)
                                                               : dataInput(link.direction);
          }
        } else {
          if (index < op.operands.size() && op.operands[index].optional)
            continue;
          throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                              "operand has no provider provenance for node " +
                                  std::to_string(node.id) + " operand " + std::to_string(index));
        }
        builder.source(static_cast<unsigned>(sink), source);
      }
      return;
    }
    if (event.kind == MaterializedEventKind::LinkLaunch) {
      if (!event.edge || !event.direction || !event.domain || !event.transportActionIndex)
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                            "link launch has incomplete provenance");
      const auto& edge = dfg.edge(*event.edge);
      if (edge.kind() == ir::Edge::Kind::Memory)
        throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                            "memory edge cannot be routed");
      const auto& dep = mappedEdge(mapping, *event.edge);
      if (!dep.transport)
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                            "link event has no transport plan");
      const auto& actions = dep.transport->actions;
      const auto actionIndex = *event.transportActionIndex;
      if (actionIndex >= actions.size())
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                            "link action index is out of range");
      std::string source;
      if (actionIndex > 0 && std::holds_alternative<VirtualHold>(actions[actionIndex - 1])) {
        const auto* segment = segmentForEdgeAtTile(
            mapping, edge.id, std::get<VirtualHold>(actions[actionIndex - 1]).tile);
        if (!segment)
          throw LoweringError(TargetLoweringStatus::UnsupportedValueSource,
                              "hold has no physical register");
        source = rfSource(segment->domain, mapping.allocationFor(segment->id).readPort == 1);
      } else if (actionIndex > 0 && std::holds_alternative<LinkStep>(actions[actionIndex - 1])) {
        const auto& prior = std::get<LinkStep>(actions[actionIndex - 1]);
        source = edge.kind() == ir::Edge::Kind::Predicate ? predicateInput(prior.direction)
                                                          : dataInput(prior.direction);
      } else {
        source = resultSource(target.operation(dfg.node(edge.src).operation).resultSource);
      }
      builder.route(*event.domain, *event.direction, source);
      ++stats.linkLaunches;
      return;
    }
    if (event.kind == MaterializedEventKind::RFWrite) {
      if (!event.segment || !event.physicalRegister || !event.edge)
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                            "RF write has incomplete provenance");
      const auto& segment = mapping.storageRequirements().segment(*event.segment);
      const auto& edge = dfg.edge(*event.edge);
      std::string source;
      for (const auto& origin : segment.origins) {
        if (origin.kind == StorageOriginKind::ExplicitVirtualHold && origin.transportActionIndex) {
          const auto& actions = mappedEdge(mapping, edge.id).transport->actions;
          if (*origin.transportActionIndex > 0 &&
              std::holds_alternative<LinkStep>(actions[*origin.transportActionIndex - 1])) {
            const auto& link = std::get<LinkStep>(actions[*origin.transportActionIndex - 1]);
            source = edge.kind() == ir::Edge::Kind::Predicate ? predicateInput(link.direction)
                                                              : dataInput(link.direction);
          } else {
            source = resultSource(target.operation(dfg.node(edge.src).operation).resultSource);
          }
          break;
        }
      }
      if (source.empty()) {
        const auto& actions = mappedEdge(mapping, edge.id).transport->actions;
        if (!actions.empty() && std::holds_alternative<LinkStep>(actions.back())) {
          const auto& link = std::get<LinkStep>(actions.back());
          source = edge.kind() == ir::Edge::Kind::Predicate ? predicateInput(link.direction)
                                                            : dataInput(link.direction);
        } else {
          source = resultSource(target.operation(dfg.node(edge.src).operation).resultSource);
        }
      }
      const auto port = mapping.allocationFor(segment.id).writePort;
      if (segment.domain == RegisterBankDomain::Data)
        builder.dataWrite(port, event.physicalRegister->index, source);
      else
        builder.predicateWrite(port, event.physicalRegister->index, source);
      ++stats.rfWrites;
      return;
    }
    if (event.kind == MaterializedEventKind::RFRead)
      ++stats.rfReads;
  }

  TargetControlCycle lowerCycle(const CycleBundle& bundle, std::uint32_t cycle) {
    TargetControlCycle result;
    result.tiles.resize(static_cast<std::size_t>(target.array().rows) * target.array().cols);
    std::vector<TileBuilder> builders(result.tiles.size());
    for (std::size_t index = 0; index < builders.size(); ++index) {
      auto& builder = builders[index];
      builder.control = target.defaultTileControl();
      builder.stats = &stats;
      builder.target = &target;
      builder.tile = TileCoord{static_cast<std::uint32_t>(index / target.array().cols),
                               static_cast<std::uint32_t>(index % target.array().cols)};
    }
    for (const auto& event : bundle.events) {
      if (event.kind == MaterializedEventKind::LiveOutBoundaryUse)
        continue;
      auto tile = event.tile;
      if (!tile && event.kind == MaterializedEventKind::NodeIssue && event.node)
        tile = mapping.staged().modulo().placement(*event.node).tile;
      if (!tile)
        throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule, "event has no tile");
      eventForTile(event, builders.at(tileIndex(*tile, target)), cycle);
    }
    for (std::size_t i = 0; i < result.tiles.size(); ++i)
      result.tiles[i] = builders[i].control;
    return result;
  }

  void eventForTile(const MaterializedEvent& e, TileBuilder& b, std::uint32_t cycle) {
    event(e, b, cycle);
  }

  TargetControlProgram run() {
    if (schedule.ii() == 0)
      throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule, "II must be positive");
    TargetControlPhase prologue;
    for (std::uint32_t cycle = 0; cycle < schedule.prologue().cycles.size(); ++cycle)
      prologue.cycles.push_back(lowerCycle(schedule.prologue().cycles[cycle], cycle));
    RepeatingTargetKernel kernel;
    kernel.repeatCount = schedule.kernel().repeatCount;
    for (std::uint32_t cycle = 0; cycle < schedule.ii(); ++cycle) {
      if (cycle < schedule.kernel().body.size())
        kernel.body.push_back(lowerCycle(schedule.kernel().body[cycle], cycle));
      else
        kernel.body.push_back(lowerCycle(CycleBundle{}, cycle));
    }
    TargetControlPhase epilogue;
    for (std::uint32_t cycle = 0; cycle < schedule.epilogue().cycles.size(); ++cycle)
      epilogue.cycles.push_back(lowerCycle(schedule.epilogue().cycles[cycle], cycle));
    stats.semanticCycles = prologue.cycles.size() + kernel.body.size() + epilogue.cycles.size();
    return TargetControlProgram(schedule.ii(), schedule.tripCount(), std::move(prologue),
                                std::move(kernel), std::move(epilogue));
  }
};

EncodedTargetProgram encodeProgram(const TargetControlProgram& program, const TargetModel& target,
                                   TargetLoweringStats& stats) {
  auto encodePhase = [&](const TargetControlPhase& phase) {
    EncodedTargetPhase result;
    for (const auto& cycle : phase.cycles) {
      EncodedTargetCycle encoded;
      for (const auto& control : cycle.tiles) {
        const auto bits = cgra::encode(control, target);
        const auto roundTrip = cgra::encode(cgra::decode(bits, target), target);
        if (roundTrip != bits)
          throw LoweringError(TargetLoweringStatus::ControlEncodingFailure,
                              "generated control failed encode/decode round trip");
        encoded.tiles.push_back(bits);
        ++stats.encodedControls;
      }
      result.cycles.push_back(std::move(encoded));
    }
    return result;
  };
  EncodedTargetProgram result;
  result.prologue = encodePhase(program.prologue());
  result.kernel.repeatCount = program.kernel().repeatCount;
  TargetControlPhase kernelPhase{program.kernel().body};
  result.kernel.body = encodePhase(kernelPhase).cycles;
  result.epilogue = encodePhase(program.epilogue());
  return result;
}

Json chunks(const EncodedControl& control) {
  Json result = Json::array();
  for (auto chunk : control.chunks) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08x", chunk);
    result.push_back(text);
  }
  return result;
}

Json buildManifestJson(const TargetDFG& dfg, const TargetModel& target,
                       const TargetControlProgram& program, const EncodedTargetProgram& encoded,
                       const TargetLoweringOptions& options) {
  (void)dfg;
  const auto prologue = encoded.prologue.cycles.size();
  const auto epilogue = encoded.epilogue.cycles.size();
  const auto ii = program.ii();
  const auto kernelRepeats = encoded.kernel.repeatCount;
  // The retained v1 replay contract requires a positive loop repeat count.
  // When T011 has no common periodic window, all semantic events are already
  // in the explicit boundary image; one idle kernel iteration preserves that
  // image without duplicating an event.
  const auto replayTripCount = kernelRepeats == 0 ? std::uint64_t{1} : kernelRepeats;
  const auto runCycles = prologue + replayTripCount * static_cast<std::uint64_t>(ii) + epilogue;
  Json root;
  root["schema"] = "cgra.program_manifest.v1";
  root["name"] = options.programName;
  root["version"] = 1;
  root["target"] =
      Json{{"schema", std::string("cgra.target.v") + std::to_string(target.contractVersion())},
           {"name", std::string(target.name())},
           {"path", options.targetPath}};
  root["run"] = Json{
      {"run_cycles", runCycles},
      {"result_observation", Json{{"mode", "trace_only"}, {"description", options.observation}}}};
  root["loop"] = Json{{"enabled", true},
                      {"prologue_cycles", prologue},
                      {"ii", ii},
                      // The v1 loop descriptor repeats the compact kernel image.  Its
                      // repeat count may differ from the source trip count when boundary
                      // instances are already represented in the prologue/epilogue.
                      {"trip_count", replayTripCount},
                      {"epilogue_cycles", epilogue}};
  Json tiles = Json::array();
  const auto addCycle = [&](Json& controls, std::uint64_t pc, const EncodedTargetCycle& cycle) {
    for (std::uint32_t row = 0; row < target.array().rows; ++row)
      for (std::uint32_t col = 0; col < target.array().cols; ++col) {
        const auto index = static_cast<std::size_t>(row) * target.array().cols + col;
        controls[row][col].push_back({{"pc", pc}, {"chunks", chunks(cycle.tiles.at(index))}});
      }
  };
  Json controls = Json::array();
  for (std::uint32_t row = 0; row < target.array().rows; ++row) {
    controls.push_back(Json::array());
    for (std::uint32_t col = 0; col < target.array().cols; ++col)
      controls[row].push_back(Json::array());
  }
  std::uint64_t pc = 0;
  for (const auto& cycle : encoded.prologue.cycles) {
    addCycle(controls, pc++, cycle);
  }
  for (const auto& cycle : encoded.kernel.body)
    addCycle(controls, pc++, cycle);
  for (const auto& cycle : encoded.epilogue.cycles)
    addCycle(controls, pc++, cycle);
  Json constants = Json::array();
  for (const auto& allocation : options.constantImage.entries) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(allocation.bits));
    constants.push_back(Json{{"addr", allocation.location.address}, {"value", text}});
  }
  Json scratchpad = Json::array();
  for (const auto& [address, value] : options.scratchpadPreload) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    scratchpad.push_back(Json{{"addr", address}, {"value", text}});
  }
  for (std::uint32_t row = 0; row < target.array().rows; ++row)
    for (std::uint32_t col = 0; col < target.array().cols; ++col)
      tiles.push_back(
          Json{{"row", row},
               {"col", col},
               {"control", controls[row][col]},
               {"const_memory", constants},
               // The scratchpad is shared by the mesh.  Its preload image
               // belongs to one canonical configuration tile, not every tile.
               {"scratchpad_preload", (row == 0 && col == 0) ? scratchpad : Json::array()}});
  root["program"] = Json{{"format", "explicit_tile_images"},
                         {"control_word_encoding", "lsb_first_32bit_chunks"},
                         {"tiles", tiles}};
  return root;
}

} // namespace

ProgramManifest ProgramManifestBuilder::build(const target::TargetDFG& dfg,
                                              const TargetModel& target,
                                              const TargetControlProgram& program,
                                              const EncodedTargetProgram& encoded,
                                              const TargetLoweringOptions& options) {
  return ProgramManifest{buildManifestJson(dfg, target, program, encoded, options).dump(2)};
}

bool TargetControlProgramVerifier::verify(const TargetDFG& dfg, const TargetModel& target,
                                          const RFAllocatedMapping& mapping,
                                          const MaterializedSchedule& schedule,
                                          const TargetControlProgram& program, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };
  if (program.ii() != schedule.ii() || program.tripCount() != schedule.tripCount())
    return fail("control program metadata does not match materialized schedule");
  const auto tileCount = static_cast<std::size_t>(target.array().rows) * target.array().cols;
  auto verifyPhase = [&](const TargetControlPhase& phase) {
    for (const auto& cycle : phase.cycles)
      if (cycle.tiles.size() != tileCount)
        return false;
    return true;
  };
  if (!verifyPhase(program.prologue()) || !verifyPhase(program.epilogue()) ||
      program.kernel().body.size() != program.ii() ||
      !verifyPhase(TargetControlPhase{program.kernel().body}))
    return fail("control phase shape is invalid");
  for (const auto& phase :
       {program.prologue(), TargetControlPhase{program.kernel().body}, program.epilogue()})
    for (const auto& cycle : phase.cycles)
      for (const auto& control : cycle.tiles)
        try {
          cgra::encode(control, target);
        } catch (const std::exception& ex) {
          return fail(ex.what());
        }
  auto verifyBundle = [&](const CycleBundle& bundle, const TargetControlCycle& control) {
    for (const auto& event : bundle.events) {
      if (event.kind == MaterializedEventKind::LiveOutBoundaryUse)
        continue;
      TileCoord tile;
      if (event.tile)
        tile = *event.tile;
      else if (event.kind == MaterializedEventKind::NodeIssue && event.node)
        tile = mapping.staged().modulo().placement(*event.node).tile;
      else
        return false;
      const auto& tileControl = control.tiles.at(tileIndex(tile, target));
      if (event.kind == MaterializedEventKind::NodeIssue) {
        if (!event.node)
          return false;
        const auto& op = target.operation(dfg.node(*event.node).operation);
        if (!op.encoding)
          return false;
        if (op.executionClass == TargetExecutionClass::FU) {
          if (tileControl.op != op.encoding->symbol)
            return false;
        } else if (tileControl.lsuOp != op.encoding->symbol) {
          return false;
        }
      } else if (event.kind == MaterializedEventKind::LinkLaunch) {
        if (!event.direction || !event.domain)
          return false;
        const auto& route = event.domain == NetworkDomain::Data
                                ? tileControl.dataRoutes[directionIndex(*event.direction)]
                                : tileControl.predicateRoutes[directionIndex(*event.direction)];
        if (!route.enabled || route.source == "NONE")
          return false;
      } else if (event.kind == MaterializedEventKind::RFWrite) {
        if (!event.physicalRegister || !event.segment)
          return false;
        const auto& segment = mapping.storageRequirements().segment(*event.segment);
        const auto& reg = *event.physicalRegister;
        const auto* bank = target.registerBank(segment.domain, reg.tile.row, reg.tile.col);
        if (!bank || reg.tile != segment.tile || reg.index >= bank->depth ||
            !bank->allocates(reg.index))
          return false;
        if (segment.domain == RegisterBankDomain::Data) {
          if (!tileControl.dataWrite0Enable && !tileControl.dataWrite1Enable)
            return false;
          if ((tileControl.dataWrite0Enable && tileControl.dataWrite0Addr != reg.index) &&
              (tileControl.dataWrite1Enable && tileControl.dataWrite1Addr != reg.index))
            return false;
        } else {
          if (!tileControl.predicateWrite0Enable && !tileControl.predicateWrite1Enable)
            return false;
          if ((tileControl.predicateWrite0Enable && tileControl.predicateWrite0Addr != reg.index) &&
              (tileControl.predicateWrite1Enable && tileControl.predicateWrite1Addr != reg.index))
            return false;
        }
      } else if (event.kind == MaterializedEventKind::RFRead) {
        if (!event.physicalRegister || !event.segment)
          return false;
        const auto& segment = mapping.storageRequirements().segment(*event.segment);
        const auto& reg = *event.physicalRegister;
        if (reg.tile != segment.tile)
          return false;
        const bool dataMatch =
            segment.domain == RegisterBankDomain::Data &&
            (tileControl.dataRfReadAddrA == reg.index || tileControl.dataRfReadAddrB == reg.index);
        const bool predicateMatch = segment.domain == RegisterBankDomain::Predicate &&
                                    (tileControl.predicateRfReadAddrA == reg.index ||
                                     tileControl.predicateRfReadAddrB == reg.index);
        if (!dataMatch && !predicateMatch)
          return false;
      }
    }
    return true;
  };
  for (std::size_t cycle = 0; cycle < schedule.prologue().cycles.size(); ++cycle)
    if (!verifyBundle(schedule.prologue().cycles[cycle], program.prologue().cycles[cycle]))
      return fail("prologue semantic event is not represented by target control");
  for (std::size_t cycle = 0; cycle < schedule.kernel().body.size(); ++cycle)
    if (!verifyBundle(schedule.kernel().body[cycle], program.kernel().body[cycle]))
      return fail("kernel semantic event is not represented by target control");
  for (std::size_t cycle = 0; cycle < schedule.epilogue().cycles.size(); ++cycle)
    if (!verifyBundle(schedule.epilogue().cycles[cycle], program.epilogue().cycles[cycle]))
      return fail("epilogue semantic event is not represented by target control");
  return true;
}

TargetLoweringResult TargetLowering::lower(const TargetDFG& dfg, const TargetModel& target,
                                           const RFAllocatedMapping& mapping,
                                           const MaterializedSchedule& schedule,
                                           const TargetLoweringOptions& options) {
  TargetLoweringResult result;
  try {
    if (!TargetDFGVerifier::verify(dfg, target).ok())
      throw LoweringError(TargetLoweringStatus::InvalidTargetDFG, "TargetDFG verification failed");
    if (!RFAllocationVerifier::verify(dfg, target, mapping).ok())
      throw LoweringError(TargetLoweringStatus::InvalidRFAllocatedMapping,
                          "RF allocation verification failed");
    ScheduleMaterializationRequest request{schedule.tripCount(), {}};
    if (!MaterializedScheduleVerifier::verify(dfg, target, mapping, request, schedule).ok())
      throw LoweringError(TargetLoweringStatus::InvalidMaterializedSchedule,
                          "materialized schedule verification failed");
    auto loweringOptions = options;
    try {
      loweringOptions.constantImage = ConstantAllocator::allocate(dfg, target);
    } catch (const std::exception& error) {
      throw LoweringError(TargetLoweringStatus::ConstantCapacityExceeded, error.what());
    }
    Lowerer lowerer{dfg, target, mapping, schedule, loweringOptions, {}};
    auto controls = lowerer.run();
    std::string verificationError;
    if (!TargetControlProgramVerifier::verify(dfg, target, mapping, schedule, controls,
                                              &verificationError))
      throw LoweringError(TargetLoweringStatus::VerificationFailure, verificationError);
    auto encoded = encodeProgram(controls, target, lowerer.stats);
    result.status = TargetLoweringStatus::Success;
    result.controls = controls;
    result.encoded = encoded;
    result.manifest =
        ProgramManifestBuilder::build(dfg, target, controls, encoded, loweringOptions);
    result.constantImage = loweringOptions.constantImage;
    result.stats = lowerer.stats;
    return result;
  } catch (const LoweringError& error) {
    result.status = error.status;
    TargetLoweringDiagnostic diagnostic;
    diagnostic.status = error.status;
    diagnostic.message = error.what();
    result.diagnostics.push_back(std::move(diagnostic));
  } catch (const std::exception& error) {
    result.status = TargetLoweringStatus::InternalError;
    TargetLoweringDiagnostic diagnostic;
    diagnostic.status = result.status;
    diagnostic.message = error.what();
    result.diagnostics.push_back(std::move(diagnostic));
  }
  return result;
}

} // namespace cgra::lowering
