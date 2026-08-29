// SPDX-License-Identifier: MIT
#include "cgra/Mapping/StageDifferenceAnalysis.h"

#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void testSignedCeil() {
  expect(cgra::mapping::ceilDivSigned(0, 4) == 0, "zero ceiling division");
  expect(cgra::mapping::ceilDivSigned(5, 4) == 2, "positive ceiling division");
  expect(cgra::mapping::ceilDivSigned(-5, 4) == -1, "negative ceiling division");
}

void testStageAndSlack() {
  cgra::target::TargetEdge edge{7, 1, 2, 0, cgra::ir::DataEdgeInfo{}};
  cgra::mapping::NodePlacement source{1, {0, 0}, cgra::mapping::ModuloSlot(3)};
  cgra::mapping::NodePlacement destination{2, {0, 1}, cgra::mapping::ModuloSlot(0)};
  cgra::mapping::MappedDependence dependence{7, cgra::ir::Edge::Kind::Data, 1, std::nullopt};
  const auto requirement = cgra::mapping::minimumStageDifference(edge, source, destination,
                                                                  dependence, 4);
  expect(requirement.minimumStageDelta == 1, "wrapped slot needs one stage");
  expect(cgra::mapping::minimumTerminalSlackCycles(requirement, source, destination, 4) == 0,
         "minimum slack should be zero for exact separation");

  edge.distance = 1;
  const auto carried = cgra::mapping::minimumStageDifference(edge, source, destination,
                                                             dependence, 4);
  expect(carried.minimumStageDelta == 0, "distance one consumes the stage wrap");
}

} // namespace

int main() {
  testSignedCeil();
  testStageAndSlack();
  return 0;
}
