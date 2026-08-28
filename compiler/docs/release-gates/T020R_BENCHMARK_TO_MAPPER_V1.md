# T020R Benchmark-to-Mapper V1 Release Record

This record is an implementation checkpoint for T-COMP-020R. It is not a
release seal until the exact feature head has passed hosted compiler,
hardware, and full CGRA-Bench gates on the pinned 32-bit environment.

## Contract

- Base candidate: `2ea91b631e3c12b94d02ad70146bf0b18eb404ed`.
- Corpus: `https://github.com/tancheng/CGRA-Bench` at
  `6729aaf225d0320e4e0d3b419e20483069a5a69b`.
- Research profiles: `target/cgra_mapping32_v1.json` and
  `target/cgra_mapping64_v1.json`; both are explicitly non-RTL mapping
  targets.
- Hardware profile: `target/cgra_v3.json`; the hardware lane remains
  32-bit, word-addressed, and requires complete encoding/lowering metadata.
- Research profiles use explicit byte-addressed Generic memory units. The
  frontend runner passes `--address-unit-bytes 1`; the hardware lane passes
  the native four-byte unit. Address units are independent of access width.
- A route candidate is L4 (`ROUTE_MAPPED`) only after
  `ModuloMappingVerifier` passes. The release mapping result is L5
  (`RF_CONSTRAINED_MAPPED`) and additionally requires the finite
  `StageScheduler`, `StageAssignmentVerifier`, `RFAllocator`, and
  `RFAllocationVerifier` checks. RF rejection is reported separately and
  never gets promoted to a mapped result.

## Implemented changes

- Added a hardware-lane guard that rejects abstract mapping targets before
  target legalization and manifest generation.
- Added explicit `mapper_invoked` pipeline telemetry. Audit reports count a
  loop as entered into the Mapper only when this producer field is true (with
  a compatibility fallback for older successful artifacts), rather than
  inferring entry from the MII tier.
- Classified `MAP_NO_MAPPING_WITHIN_II_LIMIT` as a mapping budget/profile
  limit, preserving the distinction from a genuinely exhausted search.
- Added independent Generic custom-operation signature checks for supported
  typed keys and tightened research target descriptors so declared operand and
  result types are supported, role-compatible, and directionally valid.
- Added regression coverage for the hardware/research boundary, typed
  operation corruption, Mapper-entry accounting, and budget classification.
- Made the LLVM address unit an explicit frontend option. Research targets are
  byte-addressed and the runner passes unit `1`; the executable target remains
  word-addressed with unit `4`. Producer and independent verifier use the same
  declared unit, so access width can no longer silently change address meaning.
- Added audit-only `computational_families.v1.json` selectors and a report gate
  that requires each of the seven mandatory computational families to have a
  finite-RF verified modulo mapping; initialization loops cannot satisfy this
  gate.
- Enabled the finite stage/RF completion checker in mapping-research mode. Raw
  modulo candidates, RF-rejected candidates, per-II RF rejection, and accepted
  RF-constrained mappings are now reported independently.
- Decoupled abstract memory address value types from data width and moved
  alignment policy to the target contract. The front end preserves LLVM's
  observed alignment while targets decide legality.
- Preserved checked-in benchmark case overrides when inventory regenerates the
  base manifest, including the Susan header shim.
- Rejected unsupported PHI-to-PHI conditional recurrence before Generic DFG
  construction so producer success cannot emit an invalid DFG.

## Local development evidence

The earlier local research checkpoint (before finite-RF acceptance was made
the release definition) accounted for 15 kernel directories, 34 source units,
and 128 discovered loops. Its 57 Mapper entries and 55 route-verified
candidates remain development evidence only; they must not be reported as
RF-constrained mappings. A fresh hosted run is required to record the raw
candidate count, RF rejection breakdown, accepted L5 count, and per-II
outcomes under the fixed `-m32` environment.

This native run is development evidence only. It is not the T020R release
baseline: the required hosted audit must use the fixed `-m32` toolchain and
the audited 101-loop denominator. The current local 57/60 Mapper-entry
result therefore remains an open integration gate until hosted evidence is
available.

## Required hosted seal

The release candidate must record immutable run IDs and artifacts for:

- exact feature-head compiler-fast;
- exact feature-head hardware-regression;
- exact feature-head full CGRA-Bench audit;
- PR merge-ref compiler-fast and hardware-regression;
- merged-main compiler-fast, hardware-regression, and full audit.

The hosted full audit must prove `UNKNOWN=0`, `TIMEOUT=0`, clean corpus pin,
100% source/loop reconciliation, and report the actual research32/research64
coverage, mapping outcomes, and gap ranking. No mapping or physical outcome
is filled in from this local run.

## Status

**T-COMP-020R: OPEN** pending the hosted full-audit, hardware, merge-ref, and
post-merge evidence above. T020R must not be declared closed, and T020 work
must not start, until those gates are sealed.
