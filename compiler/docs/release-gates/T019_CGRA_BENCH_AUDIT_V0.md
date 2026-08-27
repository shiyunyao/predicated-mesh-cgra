# T019 CGRA-Bench Audit V0 Release Gate

This document records the reproducible audit contract. Hosted run IDs are
filled from the immutable feature, merge-ref, and post-merge workflows; local
results are not substituted for hosted evidence.

## Pinned inputs

- Base: T018 post-merge `main` at `21861ca466abe6257591bdb7d8cc28927ad32f5c`.
- Corpus: `https://github.com/tancheng/CGRA-Bench`.
- Corpus SHA: `6729aaf225d0320e4e0d3b419e20483069a5a69b`.
- License: BSD-3-Clause.
- Scope: `kernels/` only; nested `Streaming-Bench` is out of scope.
- Target: `target/cgra_v3.json` (hash recorded in `environment.json`).

## Audit contract

The lock and cases manifests account for every kernel source. The runner uses
the fixed clang/opt-14 `-m32` profile, enumerates every innermost loop, scans
LLVM features before lowering, and invokes the production frontend and ABI
compiler. Structural audit and optional functional validation are separate;
synthetic invocations are never called functional passes. Every attempted
source/loop has a terminal stage, category, diagnostic code, and owner.

## Evidence matrix

| Evidence | Result |
| --- | --- |
| Kernel directories / source units / candidate loops | recorded by full audit |
| L0-L7 tier counts | recorded in `summary.json` |
| Frontend and ISA coverage | `frontend_coverage.*`, `isa_coverage.*` |
| MII / mapping / stage / RF metrics | per-loop `results.jsonl` when reached |
| Gap ranking and reproducers | `gap_ranking.*`, `reproducers/` |
| UNKNOWN / unclassified results | must be zero |
| Source accounting reconciliation | must pass |
| Smoke run | hosted `cgra-bench-smoke` |
| Full corpus run | hosted `cgra-bench-audit` |
| Retained T013-T018 compiler/hardware gates | must remain green |

## Hosted runs

- Feature HEAD: `<FEATURE_SHA>`; compiler-fast `<RUN_ID>`; hardware-regression `<RUN_ID>`; full audit `<RUN_ID>`.
- PR merge-ref: `<MERGE_REF_SHA>`; compiler-fast `<RUN_ID>`; hardware-regression `<RUN_ID>`.
- Merged main: `<MERGED_MAIN_SHA>`; compiler-fast `<RUN_ID>`; hardware-regression `<RUN_ID>`; post-merge audit `<RUN_ID>`.

## Local audit evidence

The reproducible local full run used the pinned submodule and the checked-in
baseline profile:

```sh
python3 tools/cgra_bench/run.py --corpus third_party/CGRA-Bench \
  --cases benchmarks/cgra-bench/cases.v1.json --target target/cgra_v3.json \
  --frontend-bin build/compiler-llvm/bin/cgra-llvm-loop-lower \
  --compile-kernel-bin build/compiler-llvm/bin/cgrac-compile-kernel \
  --out build/cgra-bench/run/full --timeout 30 --all
```

This run accounted for 15 kernel directories, 34 source translation units, and
17 candidate loops as 40 terminal results. Reconciliation passed with zero
missing/unexpected sources and zero UNKNOWN results. The local host lacks the
32-bit libc headers required by some upstream units, so those units are
explicitly classified as `BUILD/SOURCE_BUILD_FAILED`; they are not silently
reprofiled. Hosted workflow evidence remains required for the release gate.

## Known limitations

T019 does not add frontend, ISA, mapper, RF, memory, or hardware semantics. A
benchmark blocked by an unsupported operation, timeout, budget, or backend
resource limit remains accounted for and is ranked for T020. No benchmark is
edited, removed, or silently excluded to improve the measured rate.

## Release decision

**T-COMP-019: OPEN until the exact feature, merge-ref, and merged-main evidence above is filled and reconciliation reports zero UNKNOWN results.**

**Next task: T-COMP-020.**
