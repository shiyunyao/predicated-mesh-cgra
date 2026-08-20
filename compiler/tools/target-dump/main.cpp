// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetModel.h"

#include <exception>
#include <iostream>

namespace {

const char *yesNo(bool value) { return value ? "yes" : "no"; }

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: cgra-target-dump <target.json>\n";
    return 2;
  }

  try {
    const auto target = cgra::TargetModel::loadFromFile(argv[1]);
    std::cout << "Target: " << target.name() << '\n';
    std::cout << "Contract version: " << target.contractVersion() << '\n';
    std::cout << "Array: " << target.array().rows << " x "
              << target.array().cols << "\n\n";
    std::cout << "DataRF:\n  depth: " << target.dataRF().depth
              << "\n  read ports: " << target.dataRF().readPorts
              << "\n  write ports: " << target.dataRF().writePorts << "\n\n";
    std::cout << "PredicateRF:\n  depth: " << target.predicateRF().depth
              << "\n  read ports: " << target.predicateRF().readPorts
              << "\n  write ports: " << target.predicateRF().writePorts
              << "\n\n";
    std::cout << "Memory:\n  " << target.memory().model << "\n  "
              << target.memory().depth << ' ' << target.memory().addressUnit
              << "s\n  " << target.memory().ports
              << " ports\n  load latency: " << target.memory().loadLatency
              << "\n\n";
    std::cout << "Networks:\n  registered: "
              << yesNo(target.dataNetwork().registeredLinks)
              << "\n  hop latency: " << target.dataNetwork().hopLatency
              << "\n  input buffering: "
              << yesNo(target.dataNetwork().inputBuffering) << "\n\n";
    std::cout << "Control:\n  raw: " << target.controlLayout().rawWidth()
              << "\n  physical: " << target.controlLayout().physicalWidth()
              << "\n  chunks: " << target.controlLayout().chunks() << "\n\n";
    std::cout << "Loop:\n  " << target.loopExecution().model
              << "\n  rotating registers: "
              << yesNo(target.loopExecution().rotatingRegisters) << '\n';
  } catch (const std::exception &error) {
    std::cerr << "target load failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
