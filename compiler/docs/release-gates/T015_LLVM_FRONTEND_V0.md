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

Feature HEAD: `bcad773ad2c419d41651aaa3cdffbd31e5dadb80`

Exact feature-head hosted evidence:

- compiler-fast: [run 32880963656](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32880963656) — PASS
- hardware-regression: [run 32880963552](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32880963552) — PASS

The hardware job executed the retained RTL regression, shared-scratchpad
checks, modulo-loop regression, T013 compiler-generated program E2E, T014
Kernel ABI E2E, and `make llvm-frontend-e2e`. The LLVM frontend E2E covered
`.ll` and `.bc` loading, scalar `x+x` poison (`7 -> 14`, `9 -> 18`), and a
second `x*x` program through the layout-aware ABI oracle.

PR merge-ref and post-merge main evidence are recorded below after those gates
complete. No generated manifest is checked in as an oracle.

Release decision: **PENDING — merge-ref and post-merge main gates remain.**
