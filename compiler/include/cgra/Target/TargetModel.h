// SPDX-License-Identifier: MIT
#pragma once

#include "cgra/Target/ControlLayout.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgra {

struct ArrayDesc {
  unsigned rows = 0;
  unsigned cols = 0;
  unsigned dataWidth = 0;
  unsigned predicateWidth = 0;
  bool hardwareBranch = false;
};

struct RegisterFileDesc {
  unsigned depth = 0;
  unsigned readPorts = 0;
  unsigned writePorts = 0;
  std::string sameCycleReadWriteSameAddress;
  std::string sameCycleMultiwriteSameAddress;
  std::unordered_map<std::string, std::vector<std::string>> writePortSources;
};

struct InterconnectDesc {
  std::string topology;
  bool registeredLinks = false;
  unsigned hopLatency = 0;
  bool inputBuffering = false;
  bool runtimeArbitration = false;
  bool compilerRouted = false;
  bool separateResourceDomain = false;
  unsigned channelsPerDirectionPerLink = 0;
};

struct MemoryDesc {
  std::string model;
  std::string addressUnit;
  unsigned depth = 0;
  unsigned widthBits = 0;
  unsigned ports = 0;
  unsigned loadLatency = 0;
  unsigned maxIssuePerLsuPerCycle = 0;
  unsigned maxIssuePerPortPerCycle = 0;
  std::string sameAddressPolicy;
  bool runtimeStall = false;
  bool runtimeArbitration = false;
};

struct LoopExecutionDesc {
  bool supported = false;
  std::string model;
  std::vector<std::string> controlPhases;
  bool rotatingRegisters = false;
  bool loopCounterOperand = false;
  bool sameAddressRfReadWriteRecurrence = false;
};

struct LsuTileDesc {
  unsigned row = 0;
  unsigned col = 0;
  unsigned portId = 0;
};

class TargetModel {
public:
  static TargetModel loadFromFile(const std::filesystem::path& path);

  std::string_view name() const noexcept { return name_; }
  unsigned contractVersion() const noexcept { return contractVersion_; }
  const ArrayDesc& array() const noexcept { return array_; }
  const RegisterFileDesc& dataRF() const noexcept { return dataRF_; }
  const RegisterFileDesc& predicateRF() const noexcept { return predicateRF_; }
  const InterconnectDesc& dataNetwork() const noexcept { return interconnect_; }
  const InterconnectDesc& predicateNetwork() const noexcept { return interconnect_; }
  const MemoryDesc& memory() const noexcept { return memory_; }
  const LoopExecutionDesc& loopExecution() const noexcept { return loopExecution_; }
  const ControlLayout& controlLayout() const noexcept { return controlLayout_; }
  const std::vector<LsuTileDesc>& lsuTiles() const noexcept { return lsuTiles_; }

  bool tileHasLSU(unsigned row, unsigned col) const noexcept;
  unsigned encodingValue(std::string_view domain, std::string_view name) const;
  std::string encodingName(std::string_view domain, unsigned value) const;
  unsigned opcodeValue(std::string_view name) const { return encodingValue("op", name); }
  unsigned dataSourceValue(std::string_view name) const {
    return encodingValue("data_source", name);
  }
  unsigned predicateSourceValue(std::string_view name) const {
    return encodingValue("predicate_source", name);
  }

private:
  std::string name_;
  unsigned contractVersion_ = 0;
  ArrayDesc array_;
  RegisterFileDesc dataRF_;
  RegisterFileDesc predicateRF_;
  InterconnectDesc interconnect_;
  MemoryDesc memory_;
  LoopExecutionDesc loopExecution_;
  ControlLayout controlLayout_;
  std::vector<LsuTileDesc> lsuTiles_;
  std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> encodings_;
  std::unordered_map<std::string, std::unordered_map<unsigned, std::string>> reverseEncodings_;
};

} // namespace cgra
