// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgra {

struct ControlField {
  std::string name;
  unsigned lsb = 0;
  unsigned width = 0;
  std::optional<std::string> encoding;
};

class ControlLayout {
public:
  unsigned rawWidth() const noexcept { return rawWidth_; }
  unsigned physicalWidth() const noexcept { return physicalWidth_; }
  unsigned chunks() const noexcept { return chunks_; }
  unsigned chunkBits() const noexcept { return chunkBits_; }
  std::string_view chunkOrder() const noexcept { return chunkOrder_; }
  unsigned paddingLsb() const noexcept { return paddingLsb_; }
  unsigned paddingWidth() const noexcept { return paddingWidth_; }
  std::uint64_t paddingValue() const noexcept { return paddingValue_; }
  const std::vector<ControlField>& fields() const noexcept { return fields_; }
  const ControlField& field(std::string_view name) const;

private:
  friend class TargetModel;

  unsigned rawWidth_ = 0;
  unsigned physicalWidth_ = 0;
  unsigned chunks_ = 0;
  unsigned chunkBits_ = 0;
  std::string chunkOrder_;
  unsigned paddingLsb_ = 0;
  unsigned paddingWidth_ = 0;
  std::uint64_t paddingValue_ = 0;
  std::vector<ControlField> fields_;
  std::unordered_map<std::string, std::size_t> fieldIndices_;
};

struct RouteControl {
  bool enabled = false;
  std::string source = "NONE";
};

struct TileControl {
  std::string op = "NOP";
  std::string srcA = "ZERO";
  std::string srcB = "ZERO";
  std::string srcP0 = "CONST_FALSE";
  std::string srcP1 = "CONST_FALSE";
  unsigned dataRfReadAddrA = 0;
  unsigned dataRfReadAddrB = 0;
  unsigned predicateRfReadAddrA = 0;
  unsigned predicateRfReadAddrB = 0;
  bool dataWrite0Enable = false;
  unsigned dataWrite0Addr = 0;
  bool dataWrite1Enable = false;
  unsigned dataWrite1Addr = 0;
  std::string dataWrite1Source = "ZERO";
  bool predicateWrite0Enable = false;
  unsigned predicateWrite0Addr = 0;
  bool predicateWrite1Enable = false;
  unsigned predicateWrite1Addr = 0;
  std::string predicateWrite1Source = "CONST_FALSE";
  std::array<RouteControl, 4> dataRoutes;
  std::array<RouteControl, 4> predicateRoutes;
  unsigned constantAddr = 0;
  std::string lsuOp = "NONE";
  std::string lsuAddrSource = "ZERO";
  std::string lsuStoreDataSource = "ZERO";
  bool lsuCommitPredicateEnable = false;
  bool lsuCommitPredicateInvert = false;
  std::string lsuCommitPredicateSource = "CONST_FALSE";
};

struct EncodedControl {
  std::array<std::uint32_t, 4> chunks{};

  bool operator==(const EncodedControl&) const = default;
};

class TargetModel;

EncodedControl encode(const TileControl& control, const TargetModel& target);
TileControl decode(const EncodedControl& encoded, const TargetModel& target);

} // namespace cgra
