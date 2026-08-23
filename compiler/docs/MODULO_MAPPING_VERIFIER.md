# ModuloMapping Verifier

`ModuloMappingVerifier` is the independent legality oracle between the
stage-free mapping IR and later stage reconstruction. It consumes only a
verified `TargetDFG`, `TargetModel`, and completed `ModuloMapping`. It creates
its own `ModuloResourceModel` and `ResourceReservationTable`; mapper search
state, cached reservations, candidate lists, and heuristic scores are never
trusted.

The verifier checks:

* complete, unique node placements and edge realizations;
* tile/slot bounds and FU/LSU operation compatibility;
* operation occupancy and fresh modulo-resource conflicts;
* independent Data and Predicate link resources;
* producer output readiness, mesh topology, hop timing, and route continuity;
* explicit `VirtualHold` intervals for waiting, including same-tile
  dependences;
* reconstructed `requiredSeparationCycles`;
* memory RAW/WAR/WAW separation with no transport plan.

Link elapsed times are relative to the producer issue. A link launches at the
producer issue slot advanced by its elapsed value, while the resource model
contains exactly the finite `[0, II)` modulo layers. Elapsed time may exceed
II without creating an absolute schedule horizon. A value may not wait between
actions unless a `VirtualHold` records that local persistence.

The verifier deliberately does not solve stage constraints, choose a minimum
II, allocate physical RF entries, check RF ports, or search for a route. A
mapping can be legal here and still fail a later `StageScheduler` or RF
feasibility pass.

Reports use the versioned `cgra.modulo_mapping.verification.v1` JSON schema.
Diagnostics are emitted in deterministic TargetNodeId/TargetEdgeId order and
include semantic resource context where applicable.

The command-line entry point is:

```text
cgra-modulo-map-verify target_dfg.json mapping.json \
  --target target/cgra_v2.json --json-report report.json
```

Exit status `0` means valid mapping, `1` means a mapping legality error, and
`2` means input or target loading failed.
