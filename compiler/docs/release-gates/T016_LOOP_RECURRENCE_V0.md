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

## Post-release evidence hardening

Final evidence branch HEAD:
`b0abf0d31c6899704eeec61d28229a76560aef59`.

Merged `main` SHA:
`4679644c5ef79a16920eea15ac4c73e7f628f304`.

The follow-up `compiler/frontend-evidence-hardening` branch adds:

- explicit `staticTripCount` versus `KernelInvocation.tripCount` validation;
- separate loop-selection, recurrence, loop-control, provenance, and result
  artifact schemas;
- named rejection tests for PHIs with more than two incoming values,
  PHI-to-PHI recurrence use, pointer/float/vector recurrence types, and raw
  header-PHI live-outs;
- verifier corruption tests for boundary provider and offset, missing repeated
  PHI-use edges, spurious edges, wrong destination, and descriptor edge ID;
- an independent LLVM recurrence -> TargetLegalizer -> MIIAnalyzer ->
  ModuloMapper -> ModuloMappingVerifier regression;
- a scalar-induction Golden/RTL matrix: `x=7 -> 10` and `x=20 -> 23`, with
  stable Generic DFG topology and changed ABI-bound DFG/manifest hashes.

The direct one-node `%iv.next = add %iv, 1` periodic representation remains a
documented fixed-RF V0 limitation: it requires a same-address read and next
iteration write at the same periodic instant, which the target contract marks
illegal. The evidence fixture preserves the same scalar induction semantics
with a two-node periodic recurrence (`add` followed by an identity `or`) and
uses the ordinary mapped recurrence, route, and RF allocation. No MVE,
rotating register, target change, or fixed-II override is introduced.

The mapper retry correction is generic: a lower II may consume only its
deterministic share of the global search budget, leaving capacity for a less
constrained higher II. Candidate ordering is unchanged, and exhausting any
per-II share without finding a solution still produces `BudgetExceeded`, not
`Infeasible`.

Local closure evidence:

- debug CTest: PASS (19/19);
- ASan/UBSan CTest: PASS (19/19);
- `make llvm-recurrence-e2e`: PASS for seed/trip-count outputs `6`, `9`, `12`,
  `24` and induction outputs `10`, `23`, with Golden/RTL agreement.

Hosted follow-up evidence:

- exact feature-head compiler-fast: [run 32977340011](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32977340011) — PASS;
- exact feature-head hardware-regression: [run 32977340029](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32977340029) — PASS;
- PR #6 merge-ref compiler-fast: [run 32977345256](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32977345256) — PASS;
- PR #6 merge-ref hardware-regression: [run 32977345094](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32977345094) — PASS;
- post-merge main compiler-fast: [run 32979292348](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32979292348) — PASS;
- post-merge main hardware-regression: [run 32979292306](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32979292306) — PASS.

The evidence hardening is sealed on `main`; the original T016 release decision
remains **T-COMP-016: CLOSED**.
