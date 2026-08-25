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

Feature SHA, hosted compiler-fast/hardware runs, PR merge-ref runs, merged-main
runs, and the final decision are recorded here after the implementation is
complete. No generated manifest is checked in as an oracle.

Release decision: **PENDING — run hosted gates after implementation.**
