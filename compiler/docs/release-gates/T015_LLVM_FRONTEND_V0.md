# T015 LLVM Frontend V0 Release Gate

This document is the release record for the LLVM innermost-loop frontend. The
feature branch is based on the T014 merged and post-merge-green `main`.

## Scope

- LLVM 14 `.ll` and `.bc` loading through the official LLVM APIs.
- Deterministic innermost-loop selection with explicit header selection.
- Single-basic-block canonical loop control slicing.
- Target-independent scalar integer Generic DFG extraction.
- Independent LLVM frontend verification and provenance artifacts.
- LLVM → Generic DFG → Kernel ABI → existing golden/Verilator replay.

Explicitly deferred: T016 loop-carried PHI lowering, T017 predication,
T018 memory dependence, runtime ABI, zero-trip semantics, vectors, floating
point, and automatic scratchpad allocation.

## Local evidence

The semantic frontend test covers scalar arithmetic, constants, dependency
edges, deterministic JSON, verifier corruption, control-only PHI/IV, induction
data use, memory, unsupported types, and ambiguous loops. The C smoke generates
canonical LLVM with `clang-14`/`opt-14`; `make llvm-frontend-e2e` runs scalar
poison and a second arithmetic opcode through the real T014 backend, golden
model, and Verilator RTL.

## Release record

Base `main` before T015: `b29ede21ffa492f79019f6df61f6228509df67bd`

Final feature HEAD: `a8f20d68aa7f667e15e99b88ece5ee245e8befc2`

Exact feature-head hosted evidence:

- compiler-fast: [run 32882368321](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32882368321) — PASS
- hardware-regression: [run 32882368262](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32882368262) — PASS

The hardware job executed the retained RTL regression, shared-scratchpad
checks, modulo-loop regression, T013 compiler-generated program E2E, T014
Kernel ABI E2E, and `make llvm-frontend-e2e`. The LLVM frontend E2E covered
`.ll` and `.bc` loading, scalar `x+x` poison (`7 -> 14`, `9 -> 18`), and a
second `x*x` program through the layout-aware ABI oracle.

Draft PR #3 merge-ref evidence:

- compiler-fast: [run 32883740793](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32883740793) — PASS
- hardware-regression: [run 32883740806](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32883740806) — PASS

The PR was merged as `48a0de0450984245dfe8481b60d8c2d952b096ef`. The
post-merge `main` gates for that merge commit both passed:

- compiler-fast: [run 32885204074](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32885204074) — PASS
- hardware-regression: [run 32885204047](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32885204047) — PASS

The post-merge hardware job executed the retained RTL regression,
shared-scratchpad checks, modulo-loop regression, T013 compiler-generated
program E2E, T014 Kernel ABI E2E, and `make llvm-frontend-e2e`. No generated
manifest is checked in as an oracle.

Release decision: **T-COMP-015: CLOSED**

Next task: **GO T-COMP-016**

## Post-release evidence hardening

The `compiler/frontend-evidence-hardening` follow-up keeps the released T015
lowering contract unchanged while tightening composition evidence:

- `FrontendInvocationValidation` rejects a concrete invocation whose trip
  count differs from a known `staticTripCount`; unknown static trip counts
  continue to accept any positive T014 invocation count.
- `cgra-llvm-loop-lower --invocation` runs that validation before a kernel is
  handed to `cgrac-compile-kernel` and emits an explicit validation report.
- loop selection, loop-control slice, recurrence analysis, if-conversion, and
  frontend provenance artifacts now use distinct versioned schemas instead of
  repeating the complete frontend result JSON.

This hardening is evidence and composition validation, not a T015 semantic
reopening. Its source evidence commit is
`7e1e8dd33d91e1753e36f08ce5e4bf05996bf59b`; hosted merge evidence is recorded
with T016 once the follow-up branch is integrated.
