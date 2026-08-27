# CGRA-Bench Audit V0

The T019 harness audits the pinned `kernels/` corpus from
`https://github.com/tancheng/CGRA-Bench` at commit
`6729aaf225d0320e4e0d3b419e20483069a5a69b`. The corpus is a submodule; its
nested `Streaming-Bench` submodule is intentionally outside this denominator.
The BSD-3-Clause provenance and source hashes are recorded in
`benchmarks/cgra-bench/corpus.lock.json`.

## Reproduce

```sh
git submodule update --init third_party/CGRA-Bench
make cgra-bench-inventory
make cgra-bench-audit
```

The smoke gate uses the fixed six-source manifest:

```sh
make cgra-bench-smoke
```

Both commands use the production `cgra-llvm-loop-lower` and
`cgrac-compile-kernel` binaries. The runner creates an isolated directory for
each source and loop, preserves compiler logs and artifacts, and continues
after a case failure. A run records the project/corpus/target hashes,
toolchain, profile, timeout, and every terminal result in `results.jsonl`.

## Build profile

Source compilation uses `clang-14`/`clang++-14` with `-O0`,
`-Xclang -disable-O0-optnone`, `-fno-discard-value-names`, and `-m32`. LLVM is
canonicalized with `mem2reg`, `loop-simplify`, `lcssa`, and `simplifycfg`.
The 32-bit profile is deliberate: T018 defines logical scratchpad word
addresses as 32-bit values. A missing multilib toolchain is a structured
`BUILD` result; the harness never silently falls back to a different profile.

## Accounting and tiers

The lock records three denominators: kernel directories, source translation
units, and candidate loops. Each enabled source must be represented by at
least one terminal result; each discovered loop gets its own result. Results
are assigned tiers from `DISCOVERED` through `MANIFEST_COMPLETE` (and
`FUNCTIONAL_RTL_VALIDATED` only for a separately declared functional case).
Synthetic invocations are marked `synthetic: true` and are structural audit
evidence, never a benchmark functional pass.

## Failure taxonomy

The first blocking stage is classified as `BUILD`, `LLVM`, `LOOP_SELECTION`,
`FRONTEND`, `GENERIC_IR`, `INVOCATION`, `ABI`, `TARGET_ISA`, `MII`,
`MAPPING_BUDGET`, `MAPPING_INFEASIBLE`, `MAPPING_VERIFY`, `STAGE_SCHEDULE`,
`RF`, `MATERIALIZATION`, `TARGET_LOWERING`, `MANIFEST`, `RTL`, `TIMEOUT`, or
`INTERNAL`. Every result includes a diagnostic code and owner. A budget limit
is never reported as mapping infeasibility, and `UNKNOWN`/unclassified results
are a harness failure.

## Reports and reproducers

Each run emits `summary.json/csv/md`, frontend and ISA coverage reports,
`gap_ranking.json/md`, and per-case artifacts under `cases/`. The report
reconciles enabled corpus sources against represented sources. Failures should
be reproduced from the saved canonical LLVM, exact command, pinned target and
compiler hashes; source semantics are never rewritten by the harness.

To add a source override, edit `benchmarks/cgra-bench/cases.v1.json` and keep
the source enabled or provide one of the explicit exclusion reasons recorded in
the inventory contract. To update upstream, change the submodule and the pin in
`tools/cgra_bench/inventory.py` in the same change, regenerate the lock, and
record the new license/provenance in `benchmarks/cgra-bench/UPSTREAM.md`.

