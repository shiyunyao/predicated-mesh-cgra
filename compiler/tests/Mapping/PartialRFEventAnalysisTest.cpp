// SPDX-License-Identifier: MIT
#include "cgra/Mapping/PartialRFEventAnalysis.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void testDeferredFinalHold() {
  const auto target = cgra::TargetModel::loadFromFile(
      std::filesystem::path(CGRA_REPOSITORY_ROOT) / "target/cgra_v2.json");
  cgra::target::TargetDFGBuilder builder("partial", std::string(target.name()));
  builder.addNode({0, "ADD", cgra::TargetExecutionClass::FU, cgra::ir::ValueType::i32(), {}, 1,
                   1, {}, 0, std::nullopt});
  builder.addNode({1, "ADD", cgra::TargetExecutionClass::FU, cgra::ir::ValueType::i32(), {}, 1,
                   1, {}, 0, std::nullopt});
  builder.addEdge({0, 0, 1, 0, cgra::ir::DataEdgeInfo{0}});
  const auto dfg = builder.finish();

  cgra::mapping::TransportPlan plan;
  plan.edge = 0;
  plan.domain = cgra::mapping::NetworkDomain::Data;
  plan.requiredSeparationCycles = 1;
  plan.actions.emplace_back(cgra::mapping::LinkStep{
      cgra::mapping::NetworkDomain::Data, {0, 0}, cgra::mapping::Direction::East, 1});
  plan.actions.emplace_back(cgra::mapping::VirtualHold{
      cgra::mapping::NetworkDomain::Data, {0, 1}, 1, 2});
  cgra::mapping::MappedDependence dependence{
      0, cgra::ir::Edge::Kind::Data, 1, plan};
  const cgra::mapping::NodePlacement producer{0, {0, 0}, cgra::mapping::ModuloSlot(0)};
  const cgra::mapping::NodePlacement consumer{1, {0, 1}, cgra::mapping::ModuloSlot(1)};
  const auto chain = cgra::mapping::derivePartialStorageChain(
      dfg, target, dfg.edge(0), producer, consumer, dependence, 4);
  expect(chain.definiteEvents.size() == 1U,
         "zero lower-bound final hold should defer only its release read");
  expect(chain.hasDeferredFinalHoldRead, "final hold read should be deferred");
  expect(chain.definiteEvents.front().kind ==
             cgra::register_allocation::RFPortEventKind::PeriodicWrite,
         "capture write remains definite");
}

} // namespace

int main() {
  testDeferredFinalHold();
  return 0;
}
