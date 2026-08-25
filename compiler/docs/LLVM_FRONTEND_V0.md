# LLVM Innermost-Loop Frontend V0

T015 lowers a deliberately small LLVM subset into the target-independent
Generic DFG. It is the frontend boundary before Kernel ABI V0; it does not
select a PE, modulo slot, route, register, constant-memory address, or control
word.

## Accepted shape

The selected loop must be an innermost natural loop with one basic block, one
latch, one exiting conditional branch, and one exit. The branch condition is
consumed by `LoopControlSlice`. A canonical induction PHI, increment, compare,
and backedge are structural control and are omitted from the DFG.

The first supported data operations are scalar integer `add`, `sub`, `mul`,
`and`, `or`, `xor`, `shl`, `lshr`, and `ashr`. Constants become
`ConstantValue`; loop-external integer SSA values become one shared
`ExternalValue`; in-loop SSA def-use becomes a distance-zero Data edge. Values
used outside the loop become `LiveOut`, including trivial LCSSA wrappers.

## Explicit boundaries

Loop-carried PHI data use and induction-variable data use are rejected for
T016. Loads, stores, GEP, atomics, fences, calls, floating point, vectors,
internal control flow, and non-trivial exit merges are rejected with stable
frontend diagnostics. T015 does not infer memory dependence or predication.

The frontend records LLVM arithmetic flags (`nsw`, `nuw`, `exact`) in node
source provenance. Generic DFG V0 does not reinterpret those flags as machine
constraints; the invocation must remain within the non-poison semantics
accepted by the current IR contract.

## CLI and artifacts

```text
cgra-llvm-loop-lower kernel.ll \
  --function kernel --loop-header loop \
  --artifact-dir build/llvm-frontend/case \
  -o build/llvm-frontend/case/generic_dfg.json
```

`.ll` and `.bc` are accepted. `--list-loops` prints deterministic candidates.
The artifact directory contains the input module, loop metadata, control-slice
summary, provenance, Generic DFG, DFG verification, frontend result, and the
independent frontend verification report.

## End-to-end smoke

```text
make llvm-frontend-e2e
```

The target generates canonical LLVM from the checked-in C fixtures using only
`mem2reg`, `loop-simplify`, `lcssa`, and `simplifycfg`, lowers it, invokes
`cgrac-compile-kernel`, and replays generated manifests through the existing
golden/Verilator runner. It checks `x+x` for `x=7` and `x=9`, and `x*x` for
`x=7`; the output address is resolved from `KernelSignature` and
`KernelABILayout`, never hard-coded in the frontend.

## Invocation boundary

The frontend emits `ExternalValue` and `LiveOut` only. T014 specializes
invocation values, reserves ABI output words, and inserts ABI Stores. A
runtime/symbolic trip count, zero-trip semantics, PHI lowering, memory
dependence, predication, and LLVM-specific runtime ABI remain later tasks.
