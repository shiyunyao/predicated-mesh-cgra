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
| Retained T013-T018 compiler/hardware gates | PASS |

The hosted workflow rejects a full audit when reconciliation fails, or when
the report contains `UNKNOWN` or `TIMEOUT`. It verifies the pinned, clean
submodule before invoking the audit. `hardware-regression` also runs on the
T019 feature branch; its change detector treats `tools/`, `compiler/`,
`tests/`, `Makefile`, and workflow edits as backend-affecting.

## Hosted runs

- Feature HEAD `038ec38924f4945d815cb3eab320f5beac6252a6`:
  compiler-fast run `33056194064` / gate job `98464399343` PASS;
  hardware-regression run `33056194108` / backend job `98463498795` /
  gate job `98468308668` PASS; full audit run `33056194085` / job
  `98463463653` PASS. The full-audit artifact ID is `9639746550` with
  digest `sha256:6307b26445a074c9640b89e88cfffd94cf2a68033b02b5252f0aaf3e7d8abf54`.
- PR #8 merge-ref `a64ef55a24020acc83ff5cbc09f45dd127a90382`:
  compiler-fast run `33056846869` / gate job `98466580811` PASS;
  hardware-regression run `33056846884` / backend job `98465704122` /
  gate job `98471514996` PASS.
- Merged main `da6ef8e4fb8102dea75505966c3fd1c779aff528`:
  compiler-fast run `33059320555` / gate job `98476052781` PASS;
  hardware-regression run `33059320585` / backend job `98473912476` /
  gate job `98479683728` PASS; post-merge full audit run `33060026049` /
  job `98476211349` PASS. The post-merge artifact ID is `9641390418`
  with digest
  `sha256:b120143f1634f67ecb59a1c768b73caca82cb659d33a81ebb7cf9846e1513b46`.

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

## T020 input baseline

- Project: merged main `da6ef8e4fb8102dea75505966c3fd1c779aff528`.
- Corpus: `6729aaf225d0320e4e0d3b419e20483069a5a69b`.
- Target SHA256: `eedcb60d6e1ef3f452b9b559564931d565172671e62fec1423ef017e5186369d`.
- Mapping profile: `baseline`, 120-second stage timeout, `maxII=8`, node
  candidates 100000, backtracks 50000, route calls 100000, route states 10000.
- Results and gap ranking: post-merge artifact `9641390418` from run
  `33060026049`.
- Known-supported contract: `benchmarks/cgra-bench/known_supported.v1.json`
  with zero L4 cases.

## Release decision

**T-COMP-019: CLOSED.**

**GO T-COMP-020.**
