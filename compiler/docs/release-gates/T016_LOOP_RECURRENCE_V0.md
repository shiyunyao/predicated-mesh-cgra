# T016 Loop Recurrence V0 Release Gate

Status: exact feature-head evidence complete; PR merge-ref and post-merge evidence pending.

Base `main` after T015: `741b8fddb0878e0659c4c5cc87e10d5fbeae12e6`

Feature implementation SHA: `117582200341e723dd15c83a007abb8f1d9d0dfc`

## Frozen scope

T016 lowers canonical single-block integer loop-carried PHIs into the existing
Generic DFG recurrence representation. A PHI is never emitted as a Generic
node. A canonical data PHI becomes one or more distance-one Data edges from
its latch/backedge producer, each with a single iteration-offset-zero
`RecurrenceBoundary`. Constant seeds become `ConstantRef`; loop-external seeds
become the same interned `ExternalValue` used by ordinary operands.

Control-only induction PHIs remain outside the DFG. Memory, pointer, floating,
vector, multi-latch, multi-block, distance-greater-than-one, PHI-to-PHI, and
predicated recurrences remain explicit future-scope failures.

## Implemented evidence

- Canonical constant-seed reduction lowers to one Add node and a self distance-one edge.
- Canonical external-seed recurrence preserves one `ExternalValue` and its boundary.
- Mixed control/data induction retains the backedge producer and emits recurrence edges for both data use and the next PHI value.
- The independent frontend verifier checks recurrence source, destination operand, distance, boundary provider, and descriptor edge identity.
- Corruption tests reject wrong distance, wrong destination operand, and missing boundary.
- `make llvm-recurrence-e2e` replays LLVM → Generic DFG → Kernel ABI → Golden/RTL for seed/trip-count cases.

## Release evidence

Feature-head hosted evidence:

- compiler-fast: [run 32898224678](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32898224678) — PASS
- hardware-regression: [run 32898224715](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32898224715) — PASS

The hardware run executed retained RTL, T013 compiler E2E, T014 Kernel ABI
E2E, T015 LLVM frontend E2E, and `make llvm-recurrence-e2e`. The recurrence
cases covered seed/trip-count outputs 6, 9, 12, and 24 with Golden/RTL trace
agreement and layout-aware ABI observation checks.

PR merge-ref and post-merge `main` evidence will be recorded after review.

Release decision: **PENDING**

Next task after green closure: **T-COMP-017**
