# ModuloMapper V0

`ModuloMapper` is the first whole-TargetDFG heuristic mapper. It starts with
the production MII analyzer, tries each II from `MII` through `maxII`, and
returns a stage-free `ModuloMapping` only after the independent T005 verifier
accepts it.

## Search state

Each II attempt constructs a fresh `ModuloResourceModel`, reservation table,
placement map, and dependence map. The recursive search selects an unmapped
node by mapped incident degree, static degree, loop-carried degree, and stable
node ID. Candidates are compatible tile and modulo-slot pairs ordered by
locality, row, column, and slot. There are no absolute stages, iterations, RF
indices, or machine encodings in this state.

An edge becomes closed when both endpoints are placed. Data and Predicate
edges are sent to T007 with the current read-only reservation snapshot; a
successful plan is then reserved by the mapper. Memory edges receive the
target-defined ordering separation and never invoke route search. Cycles and
self-recurrences are legal because no topological readiness rule is used.

## Transactions and failure classes

Every candidate is a transaction: node footprint, newly realized dependence
plans, and route reservations are either all retained for recursive search or
all rolled back. A deeper failure increments backtracks and tries the next
deterministic candidate. A completed mapping is materialized only at the leaf
and is always checked by T005; verifier failure is reported as a compiler
correctness failure rather than hidden by backtracking.

`NoPath` from T007 rejects one candidate. `RouteBudgetExceeded` stops the whole
mapper and is never converted to infeasibility. Likewise, global candidate,
backtrack, and route-call budgets return `BudgetExceeded` and do not trigger II
escalation. II increases only after the current search is fully exhausted.

## Boundaries

V0 does not schedule stages, allocate physical RFs, perform rip-up/reroute,
share multicast trees, negotiate congestion, or prove optimality. A successful
mapping is a legal modulo placement-and-transport representation; StageScheduler
and RF feasibility remain later passes.
