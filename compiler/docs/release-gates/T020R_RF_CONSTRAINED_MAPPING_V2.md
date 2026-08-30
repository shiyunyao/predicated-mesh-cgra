# T020R RF-Constrained Mapping V2

This record defines the finite-register mapping contract for the
`benchmark-to-mapper-v1` lane. It supersedes the earlier route-only research
interpretation of L4.

This is the canonical T020R release-gate document. The earlier V1 checkpoint
has been retired; its contract and closure requirements are consolidated here.
Earlier T020 and T014-T019 documents remain as historical release evidence.

## Frozen Semantics

- L4 (`ROUTE_MAPPED`) means that `ModuloMappingVerifier` accepted a completed
  placement/routing candidate.
- L5 (`RF_CONSTRAINED_MAPPED`) additionally requires
  `StageScheduler`, `StageAssignmentVerifier`, `RFAllocator`, and
  `RFAllocationVerifier` to accept the same candidate.
- Mapping-research uses the same finite completion checker as the hardware
  lane. A rejected candidate is discarded and the Mapper continues searching,
  including at higher II values.
- Raw completed candidates, RF rejections, per-II RF rejection counts, and
  accepted finite-RF mappings are emitted separately. `known_supported.v1.json`
  freezes L5 cases only.
- `research32` and `research64` are finite abstract targets. They are not the
  RTL target and do not imply hardware executability. `cgra_v3` remains
  unchanged.

## Closure Fixes

- Abstract memory targets declare address value types independently of data
  width. A 32-bit LLVM address is legal on `research64` when it covers the
  declared scratchpad address width.
- LLVM preserves observed memory alignment in `MemoryOpInfo`; each target
  independently applies its minimum-alignment contract.
- Inventory writes a generated base case manifest and merges a pinned,
  schema-validated override manifest. Inventory is idempotent and preserves
  adapter include directories.
- Unsupported PHI-to-PHI conditional recurrences are rejected before Generic
  DFG construction, preventing producer-success/verifier-failure artifacts.

## Local Verification

The local compiler build, full CTest suite, RF-feasibility regression,
target-legalizer regression, memory-analysis regression, and Python audit
harness tests pass on this worktree. The prior native research checkpoint is
retained only as historical route-candidate evidence; it is not an L5 result.

## Hosted Seal

The release is not sealed until the exact feature SHA has hosted
`compiler-fast`, `hardware-regression`, and both full research audits with
clean corpus pin, `UNKNOWN=0`, `TIMEOUT=0`, and reconciled source/loop
denominators. The hosted report must provide the final raw/RF counts, RF
rejection breakdown, per-II outcomes, and seven-family table. No hosted run
IDs or coverage numbers are fabricated in this document.

**Release decision: OPEN pending exact-head hosted evidence.**
