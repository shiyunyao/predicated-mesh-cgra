# Stage Scheduler

`StageScheduler` is the post-mapping stage reconstruction pass. It consumes a
T005-valid, stage-free `ModuloMapping` and assigns one non-negative software
pipeline stage to each target node. It does not place nodes, change modulo
slots, reroute edges, or allocate physical registers.

For a node `v`, the logical issue time is derived from the two authoritative
mapping layers:

```
T(v) = issueSlot(v) + stage(v) * II
```

For every mapped dependence `u -> v` with distance `d` and concrete required
separation `L`, the scheduler solves:

```
T(v) + d * II >= T(u) + L
```

The equivalent stage constraint is:

```
stage(v) >= stage(u) +
  ceilDivSigned(L - (slot(v) - slot(u)), II) - d
```

The numerator is signed. The implementation uses a mathematical signed
ceiling division rather than C++'s truncating integer division, and preserves
negative deltas. Stages start at zero and are increased by Bellman-Ford-style
longest-bound relaxation. This gives the least non-negative closure for every
feasible component. A positive-weight cycle means that this concrete mapping
cannot be aligned at its selected II; the scheduler returns an explicit
`InfeasibleStageConstraints` result and does not retry or remap it.

Before scheduling, the pass runs the independent TargetDFG verifier and T005
`ModuloMappingVerifier`. After solving, `StageAssignmentVerifier` checks the
original logical-time inequality directly, including edge distance and checked
64-bit arithmetic. A successful result therefore contains no scheduler cache,
search state, or repair decisions.

The debug format is `cgra.staged_mapping.debug.v1`. It embeds the immutable
modulo mapping and stores only `{node, stage}` assignments; logical issue time
is derived and is never a serialized authority. The CLI is:

```
cgra-stage-schedule target_dfg.json modulo_mapping.json \
  --target target/cgra_v2.json -o staged_mapping.json \
  --json-report stage_report.json
```

Stage scheduling intentionally stops before stage materialization, prologue /
kernel / epilogue generation, and physical RF allocation.
