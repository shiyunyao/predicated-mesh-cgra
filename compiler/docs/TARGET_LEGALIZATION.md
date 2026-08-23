# Target Legalization and Target DFG

T-COMP-003 is the boundary between the target-independent loop IR and the
current CGRA's executable operation set. It consumes a `cgra::ir::DFG` that
has passed `DFGVerifier` and a `TargetModel` loaded from
`target/cgra_v2.json`. It produces a deterministic, still-unmapped Target
DFG.

## Pipeline

```text
Generic DFG
    -> DFGVerifier
    -> TargetLegalizer
    -> TargetDFGVerifier
    -> modulo resource model / mapper
```

The V0 legalizer is deliberately one-to-one: every Generic node becomes one
Target node, or the complete request fails. It does not emulate unsupported
operations, optimize, schedule, route, allocate registers, or emit control
words. A future legalizer may use the same `LegalizationMap` and
`genericOrigins` fields for one-to-many lowering without changing the Generic
DFG contract.

## Target operation semantics

Target operations are semantic names (`ADD`, `CMP_ULT`, `LOAD`, and so on), not
numeric control encodings. The operation descriptor comes from `TargetModel`
and supplies:

* execution class (`FU` or `LSU`);
* semantic operand roles (`data`, `predicate`, or `address`) and result role;
* issue occupancy;
* result latency, omitted for `STORE`;
* producer output readiness offset;
* memory access width for `LOAD` and `STORE`.

For the canonical target, integer FU results and comparisons have latency 1,
loads have latency 2, and all operations have issue occupancy 1. FU output
readiness is offset 0 and load output readiness is offset 2. These values are
queried from the complete `operations` descriptors in the target contract;
legalization does not duplicate them. An operation can be defined in the
contract while not executable on a concrete topology: LSU operations require
at least one enabled LSU tile, as reported by `TargetModel`.

The current `cgra.target.v2` file requires this complete `operations` section.
The older `ops` and `latencies` sections remain only as explicitly marked
`legacy_compatibility_view` data for control/RTL consumers; they are not used
to construct compiler operation semantics. A v2 target without the complete
section is rejected rather than silently receiving compiler defaults.

The current one-to-one operation selection is:

```text
Add/Sub/Mul/And/Or/Xor/Shl/LShr -> matching FU operation
ICmp EQ/NE/ULT/ULE              -> CMP_* operation
Select                          -> SELECT
Load/Store                      -> LSU LOAD/STORE
AShr and signed ICmp            -> unsupported on cgra_v2_shared4p
```

Generic-valid but target-unsupported operations therefore remain valid Generic
DFGs and fail only at this boundary. No implicit casts are inserted.

## Target DFG contents

Target node IDs are independent of Generic node IDs. Each node retains one or
more Generic origin IDs, and `LegalizationMap` maps every Generic node to a
vector of Target node IDs. Data, predicate, and memory edges are remapped with
their destination operand, distance, and `RAW`/`WAR`/`WAW` metadata unchanged.

Constants, external loop live-ins, and live-outs remain symbolic; no RF,
constant-memory, scratchpad, tile, or ABI assignment is made here.

The Target DFG contains no PE coordinates, modulo slots, pipeline stages,
routes, RF indices, memory ports, manifest fields, or numeric control-word
bits. `TargetDFGVerifier` independently checks operation descriptors, type
support, providers, provenance, edge domains, and memory-edge endpoints. It
does not prove placement, routing, or II feasibility.

## Debug schema and CLI

`cgra::target::writeJson` and `readJson` use the versioned debug schema
`cgra.target_dfg.debug.v1`. The text dump is intended for diagnostics and
reproducer artifacts; the JSON schema is a debug/test interchange format, not
the program-manifest ABI.

After configuring the compiler, the standalone pipeline is:

```bash
cmake -S compiler -B compiler/build
cmake --build compiler/build
compiler/build/bin/cgra-target-legalize \
  input.dfg.json \
  --target target/cgra_v2.json \
  -o output.target_dfg.json \
  --json-report build/legalization_report.json \
  --dump-text
```

The CLI runs both Generic and Target DFG verification. Exit status `0` means
the output was written and verified; `1` means an invalid/unsupported Generic
program; `2` means an input or parsing error; `3` means the materialized Target
DFG failed its independent verifier.
