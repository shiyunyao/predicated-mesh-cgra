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
The runner emits all S0-S16 stage records, reserved metrics and failure
reproducer schemas, per-loop artifact hashes, and exact toolchain version
strings. Source and discovered-loop reconciliation must both pass; timeout is
a hard audit failure.

## Evidence matrix

| Evidence | Result |
| --- | --- |
| Kernel directories / source units / candidate loops | 15 / 34 / 101 |
| Terminal results | 108; 4 L0 `DISCOVERED`, 104 L1 `LLVM_BUILT` |
| L2-L7 | zero in the hosted feature audit |
| Frontend and ISA coverage | `frontend_coverage.*`, `isa_coverage.*` |
| MII / mapping / stage / RF metrics | per-loop `results.jsonl` when reached |
| Gap ranking and reproducers | `gap_ranking.*`, `reproducers/` |
| UNKNOWN / timeout | 0 / 0 |
| Source and loop reconciliation | PASS; no missing or unexpected entries |
| Smoke run | hosted `cgra-bench-smoke` |
| Full corpus run | hosted `cgra-bench-audit`, automatically triggered on `compiler/cgra-bench-audit-v0` pushes |
| Retained T013-T018 compiler/hardware gates | must remain green |

The hosted workflow rejects a full audit when reconciliation fails, or when
the report contains `UNKNOWN` or `TIMEOUT`. It verifies the pinned, clean
submodule before invoking the audit. `hardware-regression` also runs on the
T019 feature branch; its change detector treats `tools/`, `compiler/`,
`tests/`, `Makefile`, and workflow edits as backend-affecting.

## Hosted runs

- Feature candidate `3865df0e80e3b681cd1d538d42e610f732443764`:
  compiler-fast run `33053874114` / gate job `98456619454` PASS;
  hardware-regression run `33053874110` / backend job `98455762521` /
  gate job `98459754380` PASS; full audit run `33053874185` / job
  `98455719145` PASS. The `cgra-bench-full-audit` artifact ID is
  `9638788097` with digest
  `sha256:32d8de3cce16b2a82635603f30f1ca9fc23b8fcf2f6df62703ca103aa7706b58`.
- PR merge-ref: `<MERGE_REF_SHA>`; compiler-fast `<RUN_ID>`; hardware-regression `<RUN_ID>`.
- Merged main: `<MERGED_MAIN_SHA>`; compiler-fast `<RUN_ID>`; hardware-regression `<RUN_ID>`; post-merge audit `<RUN_ID>`.

## Hosted feature audit

The feature audit used clang/LLVM 14.0.6 and the baseline mapping profile from
`environment.json`. It represented all 34 pinned sources and all 101 discovered
innermost loops as 108 terminal results. Four sources stopped at source build,
three built sources contained no innermost loop, and every discovered loop
received a structured frontend diagnostic. The first-blocker distribution was:

- unsupported loop shape: 81;
- unsupported memory type: 10;
- conditional recurrence unsupported: 6;
- source build failure: 4;
- no innermost loop: 3;
- non-affine address: 2;
- multiple internal branches: 1;
- path-sensitive memory order unsupported: 1.

The frontend coverage report contains 29 observed LLVM construct rows and the
gap ranking contains all eight terminal diagnostic classes. No upstream loop
reached Generic DFG generation, so ISA coverage, MII/mapping/RF metrics, L4,
and L6 are correctly empty rather than inferred. Consequently the hosted
baseline freeze records zero known-supported cases, and no upstream case is
eligible for a real L7 adapter. The S16 adapter framework remains covered by
unit tests; no synthetic case is promoted to functional validation.

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
17 candidate loops as 40 terminal results. Source and loop reconciliation
both passed with zero missing/unexpected cases, zero timeout, and zero UNKNOWN
results. The local host lacks the
32-bit libc headers required by some upstream units, so those units are
explicitly classified as `BUILD/SOURCE_BUILD_FAILED`; they are not silently
reprofiled. Hosted workflow evidence remains required for the release gate.

## Known limitations

T019 does not add frontend, ISA, mapper, RF, memory, or hardware semantics. A
benchmark blocked by an unsupported operation, timeout, budget, or backend
resource limit remains accounted for and is ranked for T020. No benchmark is
edited, removed, or silently excluded to improve the measured rate.

`known_supported.v1.json` was generated from feature audit run `33053874185`
via `tools/cgra_bench/freeze_supported.py`. The hosted run found zero loops at
L4, so its recorded baseline is empty. The functional adapter framework is
present, but the checked-in functional case manifest is empty because the same
audit found zero upstream L6 cases for which native input, Golden, and RTL
observations could be compared honestly.

## Release decision

**T-COMP-019: OPEN until the exact feature, merge-ref, and merged-main evidence above is filled and reconciliation reports zero UNKNOWN results.**

**T-COMP-020 remains blocked until this release gate is closed.**
