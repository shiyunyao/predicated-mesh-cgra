# Target lowering

T-COMP-012 lowers a verified `MaterializedSchedule` and
`RFAllocatedMapping` into semantic `TileControl` images, then uses the
existing target-driven encoder to produce `cgra.program_manifest.v1`.

The lowering boundary is deliberately search-free. Placement, routes, stages,
register assignments, and II are read-only inputs. An unsupported operand
provider, RF source/port mismatch, or conflicting field is reported as a
lowering error; the implementation never changes the mapping to make a
control image fit.

Each target operation carries typed lowering metadata: one physical control
sink per semantic operand and a result source. Legacy target files without
that block receive the canonical descriptor at target-load time, while new
target contracts can reject an incompatible binding before mapping starts.

Every cycle starts with `TargetModel::defaultTileControl()`. Events for the
same tile and cycle are merged. Repeating assignment of the same value is
idempotent; a different value for an owned field is a deterministic conflict.
Data and predicate routes remain separate, and RF write source restrictions
are checked against the selected target bank and port.

The generated semantic program preserves prologue, one II-cycle kernel body,
kernel repeat count, and epilogue. Numeric packing is delegated to the
existing `encode`/`decode` implementation. Each generated control must pass
the encode/decode round trip before a manifest is returned.

`ProgramManifestBuilder` emits the existing `cgra.program_manifest.v1`
schema. The downstream `tools/validate_program.py` remains an independent
compatibility check. Constants are assigned deterministic addresses from
their stable IR IDs and copied into each tile's target constant image.

Boundary-value injection and arbitrary external providers are intentionally
unsupported until the target ABI exposes a real source. They return explicit
status values instead of being replaced with zero or a fabricated route.

The debug CLI is:

```text
cgra-target-lower target_dfg.json rf_mapping.json materialized_schedule.json \
  --target target/cgra_v2.json -o program.json
```

The output file is a manifest handoff, not numeric control logic owned by the
lowering layer. Target lowering does not allocate registers, reroute values,
change II, or create a new manifest schema.
