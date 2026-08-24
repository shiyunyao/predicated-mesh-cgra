# Fixed RF Allocation V0

`RFAllocator` consumes a T009-verified `StagedMapping` and assigns concrete
register indices to the local-storage demand already represented by the
mapping. It never changes placement, modulo slots, routes, stages, or II.

Storage requirements are derived analytically. Each `VirtualHold` becomes a
segment from `producerLogicalIssue + captureElapsed` to
`producerLogicalIssue + releaseElapsed`. A route's terminal slack becomes a
segment from mapped arrival to the consumer logical use. Direct arrivals with
no slack create no segment, and registered-link flight time is never treated
as RF occupancy. Contiguous same-edge storage at the same tile and domain is
coalesced while retaining every origin.

The target contract supplies RF bank identity, domain, depth, allocatable
indices, tile applicability, read/write ports, and same-address read/write
policy. The allocator does not assume a fixed register count. Canonical
`cgra_v2` uses separate Data and Predicate banks, 16 allocatable registers,
two read ports, and two write ports; its same-address policy is forbidden.

V0 models fixed-address periodic reuse. A segment with duration greater than
II self-overlaps the next iteration and is rejected; it cannot be repaired by
assigning a different register to a later iteration. Duration equal to II is
allowed only for a target `ReadOldThenWriteNew` policy. Pairwise conflicts are
checked against the neighboring `-II`, `0`, and `+II` copies. Port pressure is
checked separately from register coloring, and simultaneous writes to one
physical register are always illegal.

Allocation uses deterministic greedy coloring followed by bounded exact
DSATUR search. Greedy failure is not treated as proof of infeasibility;
`RegisterDepthInfeasible` is returned only after exact exhaustion, while a
coloring budget exhaustion is reported as `BudgetExceeded`.

Every successful allocation is reconstructed by `RFAllocationVerifier`, which
reruns T009, storage analysis, bank/index checks, periodic conflicts, port
capacity, and same-address semantics. The debug format is
`cgra.rf_allocated_mapping.debug.v1`; origin queries expose the physical
register used by a particular VirtualHold or terminal slack segment.

The CLI is:

```
cgra-rf-allocate target_dfg.json staged_mapping.json \
  --target target/cgra_v2.json -o rf_mapping.json \
  --json-report rf_report.json
```

V0 deliberately does not implement rotating registers, modulo variable
expansion, spilling, rematerialization, or finite-loop seed materialization.
