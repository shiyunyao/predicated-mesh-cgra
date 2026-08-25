# T016 Loop Recurrence V0 Release Gate

Status: release sealed on main after feature-head, PR merge-ref, and post-merge evidence.

Base `main` after T015: `741b8fddb0878e0659c4c5cc87e10d5fbeae12e6`

Feature implementation SHA: `816f04f3041e72d5892a641489f58fc9fd014b56`

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

- compiler-fast: [run 32900682268](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32900682268) — PASS
- hardware-regression: [run 32900682250](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32900682250) — PASS

The hardware run executed retained RTL, T013 compiler E2E, T014 Kernel ABI
E2E, T015 LLVM frontend E2E, and `make llvm-recurrence-e2e`. The recurrence
cases covered seed/trip-count outputs 6, 9, 12, and 24 with Golden/RTL trace
agreement and layout-aware ABI observation checks.

PR #4 merge-ref hosted evidence:

- compiler-fast: [run 32900692012](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32900692012) — PASS
- hardware-regression: [run 32900692000](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32900692000) — PASS

The merge-ref hardware run executed the retained RTL, shared-memory, modulo-loop,
T013 compiler E2E, T014 Kernel ABI E2E, T015 LLVM frontend E2E, and T016
recurrence E2E. The recurrence step completed the seed/trip-count matrix and
layout-aware output checks.

Post-merge `main` evidence for merge commit `43e92650ec507d4d8d482554330dae68f19ae931`:

- compiler-fast: [run 32903650045](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32903650045) — PASS
- hardware-regression: [run 32903650074](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32903650074) — PASS

The post-merge hardware run executed the retained RTL, shared-memory,
modulo-loop, T013 compiler E2E, T014 Kernel ABI E2E, T015 LLVM frontend E2E,
and T016 recurrence E2E on the actual merged `main` commit.

Release decision: **T-COMP-016: CLOSED**

Next task: **GO T-COMP-017**
