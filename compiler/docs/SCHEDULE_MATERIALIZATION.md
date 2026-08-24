# Finite Schedule Materialization

`ScheduleMaterializer` is the T011 boundary between a verified periodic mapping and a
finite semantic execution schedule. It consumes a T010 `RFAllocatedMapping` and a
positive concrete trip count. It does not change placement, routes, stages, modulo
slots, II, or physical registers, and it emits no numeric control words.

Node, link, and RF actions are represented as compact periodic streams. A node uses
`slot + stage * II + iteration * II`; a link action uses the producer logical time plus
the mapped elapsed offset. Storage segments use their T010 write/read times and the
same fixed register for every producer iteration. Memory edges create no network or RF
events.

For a distance-`d` value edge, producer iterations `-d .. -1` represent the first
boundary consumers. T011 emits one abstract `BoundaryValueInject` for each required
seed and uses the same link/RF stream template as normal producer iterations. The
boundary provider is semantic only; target-specific injection is deferred to lowering.

Logical pre-roll may be negative. One checked global `timeOriginShift` translates all
events to non-negative cycle indices; individual events are never clamped. The finite
streams are factored into explicit prologue cycles, an exactly `II`-cycle kernel body,
and explicit epilogue cycles. The kernel body is stored once with a repeat count, so
large trip counts do not allocate one cycle bundle per iteration. Boundary and live-out
one-shot events are never repeated in the kernel.

`MaterializedScheduleVerifier` independently checks phase shape, node and route stream
coverage, recurrence boundary injections, RF provenance/timing, live-outs, and memory
edge restrictions. A successful materialization always passes this verifier.

The debug schema is `cgra.materialized_schedule.debug.v1`. The debugging CLI is:

```text
cgra-materialize-schedule target_dfg.json rf_allocated_mapping.json \
  --target target/cgra_v2.json --trip-count 64 \
  -o materialized_schedule.json --json-report materialization_report.json
```

V0 requires `tripCount >= 1`, uses a bounded explicit prologue/epilogue budget, and
does not implement zero-trip language semantics, runtime trip counts, ABI live-outs,
spills, or target lowering.
