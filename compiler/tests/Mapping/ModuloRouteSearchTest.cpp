// SPDX-License-Identifier: MIT
#include "../IR/Fixtures.h"

#include "cgra/Mapping/ModuloMappingVerifier.h"
#include "cgra/Mapping/ModuloRouteSearch.h"
#include "cgra/Target/TargetLegalizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using Json = nlohmann::json;
using namespace cgra::mapping;
const std::filesystem::path RepositoryRoot = CGRA_REPOSITORY_ROOT;

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

cgra::TargetModel loadTarget() {
  return cgra::TargetModel::loadFromFile(RepositoryRoot / "target/cgra_v2.json");
}

cgra::target::TargetDFG legalize(const cgra::ir::DFG& generic, const cgra::TargetModel& target) {
  const auto result = cgra::target::TargetLegalizer::legalize(generic, target);
  if (!result.ok())
    throw std::runtime_error(result.format());
  return *result.dfg;
}

ModuloMapping completeChainMapping(const cgra::target::TargetDFG& dfg, std::uint32_t ii,
                                   const TransportPlan& first) {
  ModuloMappingBuilder builder(dfg, ii);
  builder.place(0, {0, 0}, ModuloSlot(0));
  builder.place(1, {0, 1}, ModuloSlot(0));
  builder.place(2, {0, 2}, ModuloSlot(0));
  builder.setTransport(0, first);
  builder.setTransport(
      1, {1, NetworkDomain::Data, {LinkStep{NetworkDomain::Data, {0, 1}, Direction::East, 0}}, 1});
  return builder.finish();
}

void testAdjacentDataAndT005(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloResourceModel resources(target, 1);
  ResourceReservationTable reservations(resources);
  const auto result =
      ModuloRouteSearch::search(dfg, target, resources, reservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(result.ok(), "adjacent data route succeeds");
  expect(result.plan->requiredSeparationCycles == 1 && result.stats.resultHopCount == 1,
         "adjacent data route has one-cycle separation");
  expect(std::holds_alternative<LinkStep>(result.plan->actions.front()),
         "adjacent route contains a link");
  const auto mapping = completeChainMapping(dfg, 1, *result.plan);
  expect(ModuloMappingVerifier::verify(dfg, target, mapping).ok(),
         "adjacent route is accepted by independent T005 verifier");

  ModuloResourceModel wrappedResources(target, 4);
  ResourceReservationTable wrappedReservations(wrappedResources);
  const auto wrapped =
      ModuloRouteSearch::search(dfg, target, wrappedResources, wrappedReservations,
                                {0, {0, {0, 0}, ModuloSlot(3)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(wrapped.ok() && wrapped.plan->requiredSeparationCycles == 1 &&
             std::get<LinkStep>(wrapped.plan->actions.front()).elapsedFromProducerIssue == 0,
         "route crosses the modulo slot boundary without an absolute-time layer");
  expect(wrapped.stats.stateExpansions <= target.array().rows * target.array().cols * 4,
         "search expansions stay within the finite tile-times-II state universe");
  const auto json = result.toJson(0);
  expect(json.find("cgra.modulo_route_search.result.v1") != std::string::npos,
         "route result has versioned JSON schema");
}

void testPredicateAndLoadTiming(const cgra::TargetModel& target) {
  const auto predicateDfg = legalize(cgra::ir::fixtures::predicateSelectUnsigned(), target);
  ModuloResourceModel predicateResources(target, 1);
  ResourceReservationTable predicateReservations(predicateResources);
  const auto predicate =
      ModuloRouteSearch::search(predicateDfg, target, predicateResources, predicateReservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(predicate.ok(), "predicate route succeeds");
  expect(predicate.plan->domain == NetworkDomain::Predicate &&
             std::get<LinkStep>(predicate.plan->actions.front()).domain == NetworkDomain::Predicate,
         "predicate route uses the separate predicate network");

  const auto loadDfg = legalize(cgra::ir::fixtures::recurrence(), target);
  ModuloResourceModel loadResources(target, 2);
  ResourceReservationTable loadReservations(loadResources);
  const auto load =
      ModuloRouteSearch::search(loadDfg, target, loadResources, loadReservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(load.ok(), "load route succeeds");
  expect(load.plan->requiredSeparationCycles == 3,
         "load route starts at target-described output-ready offset");
  expect(std::get<LinkStep>(load.plan->actions.back()).elapsedFromProducerIssue == 2,
         "load route first link launches at elapsed two");

  ModuloMappingBuilder mappingBuilder(loadDfg, 2);
  mappingBuilder.place(0, {0, 0}, ModuloSlot(0));
  mappingBuilder.place(1, {0, 1}, ModuloSlot(0));
  mappingBuilder.setTransport(0, *load.plan);
  const auto recurrence =
      ModuloRouteSearch::search(loadDfg, target, loadResources, loadReservations,
                                {1, {1, {0, 1}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(recurrence.ok(), "same-tile recurrence uses explicit hold");
  mappingBuilder.setTransport(1, *recurrence.plan);
  expect(ModuloMappingVerifier::verify(loadDfg, target, mappingBuilder.finish()).ok(),
         "load and same-tile hold routes pass T005");
}

void testBlockedLinkWaitAndAlternate(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloResourceModel resources(target, 2);
  ResourceReservationTable reservations(resources);
  const auto direct =
      resources.linkResource(NetworkDomain::Data, {0, 0}, Direction::East, ModuloSlot(0));
  expect(direct.has_value(), "direct link exists");
  expect(reservations.reserve(std::vector<ResourceId>{*direct}, {ReservationOwnerKind::Edge, 91}),
         "direct link reservation succeeds");
  const auto before = reservations.owner(*direct);
  const auto waited =
      ModuloRouteSearch::search(dfg, target, resources, reservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(waited.ok(), "route waits for a later modulo link phase");
  expect(waited.stats.resultHoldCycles == 1 && waited.plan->requiredSeparationCycles == 2,
         "blocked direct link produces one explicit hold");
  expect(reservations.owner(*direct) == before, "route search does not mutate reservations");

  ModuloResourceModel alternateResources(target, 1);
  ResourceReservationTable alternateReservations(alternateResources);
  const auto east =
      alternateResources.linkResource(NetworkDomain::Data, {0, 0}, Direction::East, ModuloSlot(0));
  expect(east && alternateReservations.reserve(std::vector<ResourceId>{*east},
                                               {ReservationOwnerKind::Edge, 92}),
         "alternate direct link reservation succeeds");
  const auto alternate =
      ModuloRouteSearch::search(dfg, target, alternateResources, alternateReservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(alternate.ok() && alternate.stats.resultHopCount == 3,
         "blocked shortest link takes deterministic alternate path");
  expect(alternate.plan->requiredSeparationCycles == 3,
         "alternate path separation counts registered hops");
  const auto mapping = completeChainMapping(dfg, 1, *alternate.plan);
  expect(ModuloMappingVerifier::verify(dfg, target, mapping).ok(),
         "alternate route passes T005 verifier");
}

void testFailureAndBoundedSearch(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloResourceModel resources(target, 1);
  ResourceReservationTable reservations(resources);
  for (ResourceId id = 0; id < resources.resourceCount(); ++id) {
    if (kindOf(resources.resource(id)) == ResourceKind::DataLink)
      expect(reservations.reserve(std::vector<ResourceId>{id}, {ReservationOwnerKind::Edge, id}),
             "all data links can be reserved");
  }
  const auto noPath =
      ModuloRouteSearch::search(dfg, target, resources, reservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(noPath.status == RouteSearchStatus::NoPath && !noPath.plan,
         "exhausted finite graph reports NoPath");

  ResourceReservationTable empty(resources);
  RouteSearchOptions budget;
  budget.budget.maxStateExpansions = 0;
  budget.budget.maxQueuePushes = 1;
  const auto exceeded = ModuloRouteSearch::search(
      dfg, target, resources, empty, {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}},
      budget);
  expect(exceeded.status == RouteSearchStatus::BudgetExceeded && !exceeded.plan,
         "budget exhaustion is distinct from NoPath");

  RouteSearchOptions queueBudget;
  queueBudget.budget.maxStateExpansions = 10;
  queueBudget.budget.maxQueuePushes = 1;
  const auto queueExceeded = ModuloRouteSearch::search(
      dfg, target, resources, empty, {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}},
      queueBudget);
  expect(queueExceeded.status == RouteSearchStatus::BudgetExceeded && !queueExceeded.plan,
         "queue budget exhaustion is not misclassified as NoPath");

  RouteSearchOptions noHold;
  noHold.allowVirtualHold = false;
  ModuloResourceModel ii2(target, 2);
  ResourceReservationTable ii2Reservations(ii2);
  std::uint32_t owner = 93;
  for (ResourceId id = 0; id < ii2.resourceCount(); ++id) {
    const auto& resource = ii2.resource(id);
    if (kindOf(resource) == ResourceKind::DataLink &&
        std::get<LinkResource>(resource).slot == ModuloSlot(0))
      expect(ii2Reservations.reserve(std::vector<ResourceId>{id},
                                     {ReservationOwnerKind::Edge, owner++}),
             "no-hold phase links can be reserved");
  }
  const auto noHoldResult = ModuloRouteSearch::search(
      dfg, target, ii2, ii2Reservations,
      {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}}, noHold);
  expect(noHoldResult.status == RouteSearchStatus::NoPath,
         "disabling virtual hold rejects wait-dependent route");

  const auto memoryDfg = legalize(cgra::ir::fixtures::memoryDependence(), target);
  ModuloResourceModel memoryResources(target, 1);
  ResourceReservationTable memoryReservations(memoryResources);
  const auto memory =
      ModuloRouteSearch::search(memoryDfg, target, memoryResources, memoryReservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {1, 0}, ModuloSlot(0)}});
  expect(memory.status == RouteSearchStatus::UnsupportedEdge,
         "memory dependence is not sent through the network");
}

void testTargetDrivenHopLatency() {
  std::ifstream input(RepositoryRoot / "target/cgra_v2.json");
  Json json;
  input >> json;
  json["interconnect"]["hop_latency"] = 2;
  json["parameters"]["mesh_hop_latency"] = 2;
  const auto path = std::filesystem::temp_directory_path() / "cgra-route-search-hop-target.json";
  {
    std::ofstream output(path);
    output << json.dump(2) << '\n';
  }
  const auto target = cgra::TargetModel::loadFromFile(path);
  std::filesystem::remove(path);
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloResourceModel resources(target, 1);
  ResourceReservationTable reservations(resources);
  const auto result =
      ModuloRouteSearch::search(dfg, target, resources, reservations,
                                {0, {0, {0, 0}, ModuloSlot(0)}, {1, {0, 1}, ModuloSlot(0)}});
  expect(result.ok() && result.plan->requiredSeparationCycles == 2,
         "route hop latency comes from the target contract");
}

void testBoundedAlternatives(const cgra::TargetModel& target) {
  const auto dfg = legalize(cgra::ir::fixtures::arithmeticChain(), target);
  ModuloResourceModel resources(target, 1);
  ResourceReservationTable reservations(resources);
  const RouteSearchRequest request{0, {0, {0, 0}, ModuloSlot(0)},
                                   {1, {0, 1}, ModuloSlot(0)}};
  const auto canonical = ModuloRouteSearch::search(dfg, target, resources, reservations, request);
  expect(canonical.ok(), "canonical route exists for alternatives test");
  const auto one = ModuloRouteSearch::searchAlternatives(
      dfg, target, resources, reservations, {request, 1});
  expect(one.status == RouteSearchStatus::Success && one.plans.size() == 1 &&
             one.plans.front() == *canonical.plan,
         "K=1 alternatives preserves the canonical route");
  const auto many = ModuloRouteSearch::searchAlternatives(
      dfg, target, resources, reservations, {request, 3});
  expect(many.status == RouteSearchStatus::Success && !many.plans.empty() &&
             many.plans.size() <= 3,
         "bounded alternatives returns at most K valid plans");
  for (std::size_t index = 0; index < many.plans.size(); ++index)
    for (std::size_t other = index + 1; other < many.plans.size(); ++other)
      expect(many.plans[index] != many.plans[other], "route alternatives are deduplicated");
}

} // namespace

int main() {
  try {
    const auto target = loadTarget();
    testAdjacentDataAndT005(target);
    testPredicateAndLoadTiming(target);
    testBlockedLinkWaitAndAlternate(target);
    testFailureAndBoundedSearch(target);
    testTargetDrivenHopLatency();
    testBoundedAlternatives(target);
    std::cout << "modulo route search tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "modulo route search tests failed: " << error.what() << '\n';
    return 1;
  }
}
