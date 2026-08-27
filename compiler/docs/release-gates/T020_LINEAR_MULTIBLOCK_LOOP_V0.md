# T020 Canonical Linear Multi-Block Loop V0

## Release status

- Required base: `49f1d5e3d26b9acdc7a7b47fffca3de566836bf1`
- Implementation commit: `e3598dcccc48e8205b2d603950b15dac4589c12f`
- Review and release-gate hardening commit: `ce0fd4bb569355ad478012c781824b8698cef644`
- CGRA-Bench commit: `6729aaf225d0320e4e0d3b419e20483069a5a69b`
- Target: `target/cgra_v3.json`
- Release decision: **STOP - T020 remains open**

The target-independent linear CFG lowering is implemented and locally verified. The
mandatory CGRA-Bench outcome and canonical production-backend E2E gates are not
satisfied, so this branch must not be merged and T020 must not be reported closed.

## Generic CFG contract

`CanonicalLinearLoopV0` accepts an innermost natural loop only when LLVM analysis
proves all of the following:

- one canonical preheader, latch, exiting block, and exit block;
- exactly one conditional loop-termination branch;
- zero internal conditional branches;
- every other in-loop terminator is an unconditional in-loop branch;
- removing the exit edge and latch-to-header backedge leaves one directed path from
  header to latch;
- every non-header block has one in-loop predecessor;
- no non-header PHI exists.

The descriptor and lowering use CFG path order, not LLVM textual order, block names,
function names, source paths, or benchmark identity. It supports pre-test and
post-test placement when the same structural contract holds.

The following remain structured failures: missing preheader, multiple latches or
exits, break/continue, switch and indirect terminators, side entries, internal
fork/merge regions, non-header PHIs, and additional cycles. A supported T017
diamond or triangle continues through if-conversion and is not classified as a
linear loop.

## Semantics and verification

The generalized structured lowerer reuses the existing T016 recurrence and T018
memory semantics. It adds no Generic CFG node and makes no target, mapper, RF,
lowering, RTL, or target-model changes.

The independent frontend verifier reconstructs the CFG path from LLVM predecessor
and successor relationships. It does not call the producer's linear-loop analyzer.
It checks structural provenance, all-block instruction completeness, cross-block
distance-zero SSA, recurrence source and boundary provenance, and exclusion of the
termination slice.

The semantic corpus covers:

- 2, 3, 4, and 5-block linear loops;
- pre-test and post-test termination;
- cross-block data edges and exact destination operands;
- cross-block recurrence and memory order in both directions;
- function, block, and SSA renaming;
- LLVM textual block reordering;
- deterministic property seeds `0`, `1`, `7`, `19`, and `42`;
- missing cross-block edges, missing body nodes, externalized body values, wrong
  provenance, wrong recurrence source, and fake control nodes;
- missing preheader, multiple exits, switch termination, internal diamond, and
  non-header PHI rejection.

## Local verification

At review hardening commit `ce0fd4bb569355ad478012c781824b8698cef644`:

- compiler build: PASS;
- CTest: 23/23 PASS;
- CGRA-Bench harness tests: 45/45 PASS;
- canonical multi-block C frontend smoke: PASS;
- supplemental RF-feasible multi-block Golden/RTL reachability: PASS.

The supplemental RTL cases deliberately have additional arithmetic that changes
their RF lifetime shape. They are retained only as backend-reachability regression
and are not evidence for the mandatory canonical E2E kernels.

## Canonical C evidence and blockers

The exact canonical vector-add source

```c
for (unsigned i = 0; i < n; ++i)
  C[i] = A[i] + B[i];
```

lowers from clang-14 canonical LLVM as a two-block `linear_multiblock` loop. Its
Generic DFG contains real affine address dataflow, two Loads, one Add, one Store,
and induction recurrence edges. Production `cgrac-compile-kernel` was attempted at
the fixed T018/T019 backend interface and exhausted mapping budget after RF
self-overlap rejections. The task freezes Mapper and RF behavior, so no
benchmark-driven backend change was made.

The exact canonical recurrence source

```c
for (unsigned i = 0; i < n; ++i)
  value += 1;
return value;
```

also lowers as a two-block linear loop and produces the expected distance-one
recurrence. It does not produce a LiveOut because the returned value is a raw
header-PHI exit semantic currently rejected by the frozen T016 PHI LiveOut policy.
Consequently neither exact mandatory kernel currently completes the required
canonical C-to-RTL gate.

## Fixed-corpus audit

T019 before T020:

- kernel directories: 15;
- source translation units: 34;
- candidate loops: 101;
- `L2 FRONTEND_DFG`: 0;
- `L4 MAPPED`: 0;
- `LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE`: 81.

The current frontend was replayed over the 101 canonical LLVM loop artifacts from
hosted T019 audit run `33060026049` (artifact `9641390418`). Five loops reached
successful Generic DFG lowering:

- `bicg_int/kernel_bicg_int::for.cond`;
- `dtw/initializeb::for.cond`;
- `fft_int/main::for.cond`;
- `fir_int/kernel::for.cond`;
- `relu_int/main::for.cond`.

The remaining frontend terminal diagnostics were:

| Diagnostic | Loops |
| --- | ---: |
| `LLVM_FRONTEND_UNSUPPORTED_MEMORY_TYPE` | 43 |
| `LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER` | 25 |
| `LLVM_FRONTEND_UNSUPPORTED_NON_AFFINE_ADDRESS` | 12 |
| `LLVM_FRONTEND_UNSUPPORTED_IF_SIDE_EFFECT` | 7 |
| `LLVM_FRONTEND_CONDITIONAL_RECURRENCE_UNSUPPORTED` | 7 |
| `LLVM_FRONTEND_UNSUPPORTED_PATH_SENSITIVE_MEMORY_ORDER` | 1 |
| `LLVM_FRONTEND_MULTIPLE_INTERNAL_BRANCHES` | 1 |

These counts total 101 with the five successes. The 25 no-preheader cases have
multiple outside predecessors and therefore do not satisfy the frozen
`CanonicalLinearLoopV0` contract. The other blockers are explicitly outside T020.

This replay establishes `L2 = 5`, below the mandatory `L2 >= 10` gate. It does not
establish `L4 >= 8` or five mapped kernel directories. The audit report now emits
machine-readable cumulative T020 outcomes, and the hosted full-audit workflow
enforces all three thresholds plus zero shape rejection among proven linear
candidates. The expected result for the current candidate is therefore a failed
T020 outcome gate, not a release pass.

## Hosted release evidence

No exact-feature hosted release seal, PR merge-ref, or post-merge-main evidence is
recorded because the mandatory outcome gate is already known to fail. Creating a
merge-ready PR or merging this branch would contradict the task's STOP rule.

## Decision

The local CFG implementation is usable and regression-covered, but the task's
integration hypothesis was not confirmed at its specified thresholds:

```text
T-COMP-020: OPEN
STOP - remaining T020 blockers
```

T020 may close only after the canonical E2E and fixed-corpus `10 / 8 / 5` outcome
gates are met without expanding this task into wider memory types, non-affine
addressing, conditional recurrence, Mapper changes, RF changes, or benchmark
special cases.
