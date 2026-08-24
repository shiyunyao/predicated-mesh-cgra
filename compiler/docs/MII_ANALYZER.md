# MII Analyzer

`MIIAnalyzer` computes a deterministic lower bound for a verified TargetDFG:

```
MII = max(ResMII, RecMII)
```

It is an analysis only. It does not place nodes, route edges, assign stages, or
prove that an II can be mapped.

## Resource MII

V0 reports four resource components:

- `self_occupancy`: the maximum target issue occupancy of any node;
- `fu`: aggregate FU issue demand divided by the number of FU tiles;
- `lsu`: aggregate LSU issue demand divided by the number of enabled LSU tiles;
- `per_operation`: the largest demand bound for one target operation.

All divisions use checked integer ceiling division. A node with occupancy `R`
therefore requires `II >= R` even when many compatible tiles exist, because a
static modulo placement reuses its selected execution resource every iteration.

Resource counts and per-operation compatible-resource queries come from
`TargetModel`; the current v2 contract has homogeneous FU capabilities and
per-tile LSU capabilities. The query is the extension point for future
heterogeneous operation/tile capability metadata.

V0 deliberately excludes mesh-link congestion, RF pressure, constant-memory
ports, and memory-bank alias analysis. Those costs depend on placement or later
pipeline information and are not guessed here.

## Recurrence MII

Each TargetDFG dependence contributes an intrinsic separation:

- Data and predicate edges use the producer operation's `resultLatency`.
- Memory edges use `TargetModel::memoryDependenceSeparation`.

For an edge `u -> v` with distance `d`, the analyzer checks:

```
T(v) >= T(u) + intrinsic_separation - d * II
```

Candidate II values are tested with longest-path difference-constraint
relaxation. A positive cycle means that candidate is too small. The analyzer
does not enumerate simple cycles, use floating-point ratios, or use
`producerOutputReadyOffset`; that offset belongs to concrete routed transport
verification, while `resultLatency` is the intrinsic pre-mapping dependence
latency.

A positive cycle containing only distance-zero edges cannot be repaired by
increasing II. Such a graph returns
`unschedulable_zero_distance_cycle` with a deterministic edge witness.

## Result and CLI

Results preserve the separate resource and recurrence breakdowns and can be
serialized as `cgra.mii.analysis.v1`:

```sh
cgrac-mii-analyze target_dfg.json \
  --target target/cgra_v2.json \
  --json-report mii.json
```

Exit codes are `0` for success, `1` for an invalid/unschedulable graph or
missing compatible resource, `2` for input errors, `3` for target-contract
errors, and `4` for internal arithmetic or implementation errors.

The analyzer runs `TargetDFGVerifier` first and never mutates the TargetDFG.
The resulting MII is a lower bound; the mapper may need to try larger values
because placement, routing, storage, or later stage constraints can still fail.
