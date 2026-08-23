# Modulo Route Search

`ModuloRouteSearch` is the bounded single-dependence routing primitive used by
the future modulo mapper. It receives a verified `TargetDFG`, fixed producer and
consumer placements, a `ModuloResourceModel`, and a read-only
`ResourceReservationTable`. It returns one candidate `TransportPlan`; it never
places nodes, commits reservations, changes II, or performs global backtracking.

## Finite state space

The search state is `(tile, modulo slot)`. The slot is always in `[0, II)` and
the dense state universe contains exactly `array.rows * array.cols * II`
states. Absolute logical time is retained only as a shortest-path cost and in
the reconstructed relative action timestamps. A later arrival at the same
state is dominated, so the search cannot wander through repeated full-II
cycles.

Data and Predicate edges select their corresponding physical network. Memory
edges are ordering constraints and return `UnsupportedEdge`.

## Transitions and timing

The initial availability time is the producer placement issue time plus the
target operation's `producerOutputReadyOffset`. A LinkStep launches at the
current elapsed time and reserves the directed link at the producer issue slot
advanced by that elapsed time. Its arrival is delayed by the selected target
network's `hopLatency`.

Waiting is never implicit. A one-cycle Hold transition represents capture into
virtual local storage and advances both elapsed time and modulo phase by one.
Consecutive holds are coalesced into one `VirtualHold` during reconstruction.
Physical RF allocation is deliberately deferred to later scheduling stages.

The goal is the consumer tile, not the consumer modulo slot. StageScheduler
later resolves stage and dependence-distance constraints. Distinct nodes on the
same tile require an explicit one-cycle hold in V0; an empty plan is never
returned for that case.

## Search and budgets

T007 uses deterministic Dijkstra ordering:

1. minimum elapsed separation;
2. minimum hold cycles;
3. minimum link hops;
4. dense state ID and stable transition order (`North`, `East`, `South`, `West`, `Hold`).

Existing reservations are hard constraints. The input table is never modified,
and multicast, rip-up, negotiated congestion, RF capacity, and route sharing
are out of scope.

`NoPath` means the finite state graph was exhausted. `BudgetExceeded` means the
search stopped before proving that no path exists. `RouteSearchStats` records
state expansions, queue pushes, blocked links, transition counts, and result
costs for reproducers and later CI metrics.

Every successful fixture embeds the returned plan in a complete `ModuloMapping`
and checks it with the independent T005 `ModuloMappingVerifier`.
