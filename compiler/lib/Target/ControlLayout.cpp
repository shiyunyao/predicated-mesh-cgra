// SPDX-License-Identifier: MIT
#include "cgra/Target/ControlLayout.h"

#include "cgra/Target/TargetModel.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace cgra {
namespace {

constexpr std::array<std::string_view, 4> Directions = {
    "north", "south", "east", "west"};

std::uint64_t fieldLimit(unsigned width) {
  return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << width) - 1;
}

void insertBits(EncodedControl &encoded, unsigned lsb, unsigned width,
                std::uint64_t value, std::string_view fieldName) {
  if (value > fieldLimit(width))
    throw std::runtime_error("control field " + std::string(fieldName) +
                             " value does not fit width " +
                             std::to_string(width));
  for (unsigned bit = 0; bit < width; ++bit) {
    const unsigned physicalBit = lsb + bit;
    const unsigned chunk = physicalBit / 32;
    const unsigned chunkBit = physicalBit % 32;
    if ((value >> bit) & 1U)
      encoded.chunks.at(chunk) |= std::uint32_t{1} << chunkBit;
  }
}

std::uint64_t extractBits(const EncodedControl &encoded, unsigned lsb,
                          unsigned width) {
  std::uint64_t value = 0;
  for (unsigned bit = 0; bit < width; ++bit) {
    const unsigned physicalBit = lsb + bit;
    const unsigned chunk = physicalBit / 32;
    const unsigned chunkBit = physicalBit % 32;
    value |= std::uint64_t{(encoded.chunks.at(chunk) >> chunkBit) & 1U} << bit;
  }
  return value;
}

std::uint64_t semanticInteger(const TileControl &control,
                              std::string_view field) {
  if (field == "data_rf_raddr_a") return control.dataRfReadAddrA;
  if (field == "data_rf_raddr_b") return control.dataRfReadAddrB;
  if (field == "pred_rf_raddr_a") return control.predicateRfReadAddrA;
  if (field == "pred_rf_raddr_b") return control.predicateRfReadAddrB;
  if (field == "data_w0_we") return control.dataWrite0Enable;
  if (field == "data_w0_addr") return control.dataWrite0Addr;
  if (field == "data_w1_we") return control.dataWrite1Enable;
  if (field == "data_w1_addr") return control.dataWrite1Addr;
  if (field == "pred_w0_we") return control.predicateWrite0Enable;
  if (field == "pred_w0_addr") return control.predicateWrite0Addr;
  if (field == "pred_w1_we") return control.predicateWrite1Enable;
  if (field == "pred_w1_addr") return control.predicateWrite1Addr;
  if (field == "const_addr") return control.constantAddr;
  if (field == "lsu_commit_pred_enable") return control.lsuCommitPredicateEnable;
  if (field == "lsu_commit_pred_invert") return control.lsuCommitPredicateInvert;
  for (std::size_t index = 0; index < Directions.size(); ++index) {
    if (field == "data_route_" + std::string(Directions[index]) + "_we")
      return control.dataRoutes[index].enabled;
    if (field == "pred_route_" + std::string(Directions[index]) + "_we")
      return control.predicateRoutes[index].enabled;
  }
  throw std::runtime_error("unsupported semantic integer control field: " +
                           std::string(field));
}

std::string_view semanticEncoding(const TileControl &control,
                                  std::string_view field) {
  if (field == "op") return control.op;
  if (field == "src_a") return control.srcA;
  if (field == "src_b") return control.srcB;
  if (field == "src_p0") return control.srcP0;
  if (field == "src_p1") return control.srcP1;
  if (field == "data_w1_src") return control.dataWrite1Source;
  if (field == "pred_w1_src") return control.predicateWrite1Source;
  if (field == "lsu_op") return control.lsuOp;
  if (field == "lsu_addr_src") return control.lsuAddrSource;
  if (field == "lsu_store_data_src") return control.lsuStoreDataSource;
  if (field == "lsu_commit_pred_src") return control.lsuCommitPredicateSource;
  for (std::size_t index = 0; index < Directions.size(); ++index) {
    if (field == "data_route_" + std::string(Directions[index]) + "_src")
      return control.dataRoutes[index].source;
    if (field == "pred_route_" + std::string(Directions[index]) + "_src")
      return control.predicateRoutes[index].source;
  }
  throw std::runtime_error("unsupported semantic encoded control field: " +
                           std::string(field));
}

void setSemanticInteger(TileControl &control, std::string_view field,
                        std::uint64_t value) {
  if (field == "data_rf_raddr_a") control.dataRfReadAddrA = value;
  else if (field == "data_rf_raddr_b") control.dataRfReadAddrB = value;
  else if (field == "pred_rf_raddr_a") control.predicateRfReadAddrA = value;
  else if (field == "pred_rf_raddr_b") control.predicateRfReadAddrB = value;
  else if (field == "data_w0_we") control.dataWrite0Enable = value;
  else if (field == "data_w0_addr") control.dataWrite0Addr = value;
  else if (field == "data_w1_we") control.dataWrite1Enable = value;
  else if (field == "data_w1_addr") control.dataWrite1Addr = value;
  else if (field == "pred_w0_we") control.predicateWrite0Enable = value;
  else if (field == "pred_w0_addr") control.predicateWrite0Addr = value;
  else if (field == "pred_w1_we") control.predicateWrite1Enable = value;
  else if (field == "pred_w1_addr") control.predicateWrite1Addr = value;
  else if (field == "const_addr") control.constantAddr = value;
  else if (field == "lsu_commit_pred_enable") control.lsuCommitPredicateEnable = value;
  else if (field == "lsu_commit_pred_invert") control.lsuCommitPredicateInvert = value;
  else {
    for (std::size_t index = 0; index < Directions.size(); ++index) {
      if (field == "data_route_" + std::string(Directions[index]) + "_we") {
        control.dataRoutes[index].enabled = value;
        return;
      }
      if (field == "pred_route_" + std::string(Directions[index]) + "_we") {
        control.predicateRoutes[index].enabled = value;
        return;
      }
    }
    throw std::runtime_error("unsupported semantic integer control field: " +
                             std::string(field));
  }
}

void setSemanticEncoding(TileControl &control, std::string_view field,
                         std::string value) {
  if (field == "op") control.op = std::move(value);
  else if (field == "src_a") control.srcA = std::move(value);
  else if (field == "src_b") control.srcB = std::move(value);
  else if (field == "src_p0") control.srcP0 = std::move(value);
  else if (field == "src_p1") control.srcP1 = std::move(value);
  else if (field == "data_w1_src") control.dataWrite1Source = std::move(value);
  else if (field == "pred_w1_src") control.predicateWrite1Source = std::move(value);
  else if (field == "lsu_op") control.lsuOp = std::move(value);
  else if (field == "lsu_addr_src") control.lsuAddrSource = std::move(value);
  else if (field == "lsu_store_data_src") control.lsuStoreDataSource = std::move(value);
  else if (field == "lsu_commit_pred_src") control.lsuCommitPredicateSource = std::move(value);
  else {
    for (std::size_t index = 0; index < Directions.size(); ++index) {
      if (field == "data_route_" + std::string(Directions[index]) + "_src") {
        control.dataRoutes[index].source = std::move(value);
        return;
      }
      if (field == "pred_route_" + std::string(Directions[index]) + "_src") {
        control.predicateRoutes[index].source = std::move(value);
        return;
      }
    }
    throw std::runtime_error("unsupported semantic encoded control field: " +
                             std::string(field));
  }
}

} // namespace

const ControlField &ControlLayout::field(std::string_view name) const {
  const auto it = fieldIndices_.find(std::string(name));
  if (it == fieldIndices_.end())
    throw std::runtime_error("unknown target control field: " + std::string(name));
  return fields_.at(it->second);
}

EncodedControl encode(const TileControl &control, const TargetModel &target) {
  EncodedControl encoded;
  for (const auto &field : target.controlLayout().fields()) {
    const auto value = field.encoding
                           ? target.encodingValue(*field.encoding,
                                                  semanticEncoding(control, field.name))
                           : semanticInteger(control, field.name);
    insertBits(encoded, field.lsb, field.width, value, field.name);
  }
  if (extractBits(encoded, target.controlLayout().paddingLsb(),
                  target.controlLayout().paddingWidth()) != 0)
    throw std::runtime_error("encoded control has non-zero padding bits");
  return encoded;
}

TileControl decode(const EncodedControl &encoded, const TargetModel &target) {
  if (extractBits(encoded, target.controlLayout().paddingLsb(),
                  target.controlLayout().paddingWidth()) !=
      target.controlLayout().paddingValue())
    throw std::runtime_error("control word padding bits must be zero");

  TileControl control;
  for (const auto &field : target.controlLayout().fields()) {
    const auto value = extractBits(encoded, field.lsb, field.width);
    if (field.encoding) {
      setSemanticEncoding(control, field.name,
                          target.encodingName(*field.encoding, value));
    } else {
      setSemanticInteger(control, field.name, value);
    }
  }
  return control;
}

} // namespace cgra
