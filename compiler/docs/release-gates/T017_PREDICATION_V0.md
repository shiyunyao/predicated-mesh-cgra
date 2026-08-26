# T017 Predication V0 Release Gate

Status: implementation in progress; release is blocked pending the predicated
Store hardware gate and hosted evidence.

Base `main`: `90ca9b77e371201a1cf9f358ca63c7a78d463bcc`
Feature branch: `compiler/predication-v0`
Feature candidate SHA: `7e6893566069cb720f9ebedb82be70dae1bc898b`

## Implemented scope

The LLVM frontend now recognizes one structured in-loop conditional branch or
direct `select`, lowers its `icmp` to a Generic predicate, lowers merge PHIs to
Generic `Select`, and preserves branch-local pure arithmetic for speculative
execution. False-arm Store orientation is normalized by an equivalent
target-independent predicate complement.

The narrow conditional Store path accepts one direct loop-external pointer
argument, emits a Generic Store with a PredicateEdge on operand 2, and adds a
distance-one self-WAW memory edge. Loads, GEPs, multiple Stores, nested or
multiple internal branches, and calls in speculative arms remain structured
T017 failures for their respective later milestones.

`LLVMFrontendVerifier` independently checks predicate provenance and polarity,
compare operands, Select arm providers, Store address/data providers, commit
predicate, and Store self-WAW distance.

## Local evidence

- `cgra-llvm-frontend-tests`: PASS.
- Full `ctest --test-dir compiler/build/ci-debug`: PASS (18/18).
- `make llvm-predication-e2e`: PASS for compiler-generated LLVM value-merge
  cases, including Golden/Verilator replay and layout-aware ABI observation.
- Direct predicated-Store lowering and verifier corruption tests: PASS.

## Remaining release blockers

- A recurrence-driven, per-iteration predicated Store has not yet produced a
  backend-feasible manifest on the current target. The production mapper
  exhausts its bounded search with RF self-overlap rejections; no mapper or
  target shortcut is permitted to hide this result.
- Hosted feature-branch compiler-fast and hardware-regression runs have not
  been obtained for this uncommitted candidate. The workflow push trigger now
  includes `compiler/predication-v0`; run IDs must be recorded after pushing a
  final commit.
- PR merge-ref and post-merge `main` checks remain pending.

## Release decision

**STOP — remaining T017 blocker.** Do not begin T018 until the predicated Store
Golden/RTL path, hosted gates, and a fresh source audit are green.
