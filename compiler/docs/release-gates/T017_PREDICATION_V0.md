# T017 Predication V0 Release Gate

Status: implementation in progress; release is blocked pending the predicated
Store hardware gate and hosted evidence.

Base `main`: `90ca9b77e371201a1cf9f358ca63c7a78d463bcc`
Feature branch: `compiler/predication-v0`
Feature candidate SHA: `f975260805cdcf421b503410d7712693327182dc`

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

Exact-head hosted evidence for candidate `f975260805cdcf421b503410d7712693327182dc`:

- compiler-fast: [run 32944316063](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32944316063) — PASS.
- hardware-regression: [run 32944316067](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32944316067) — PASS. The run executed retained RTL, shared scratchpad, modulo-loop, T013/T014/T015/T016 E2E, and the LLVM predication value-merge E2E.

Draft PR merge-ref evidence for PR [#5](https://github.com/shiyunyao/predicated-mesh-cgra/pull/5), based on the same source candidate:

- compiler-fast: [run 32947696038](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32947696038) — PASS.
- hardware-regression: [run 32947696022](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32947696022) — PASS, including the LLVM predication V0 E2E.

## Remaining release blockers

- A recurrence-driven, per-iteration predicated Store has not yet produced a
  backend-feasible manifest on the current target. The production mapper
  exhausts its bounded search with RF self-overlap rejections; no mapper or
  target shortcut is permitted to hide this result.
- Hosted feature-branch compiler-fast and hardware-regression runs have not
  been obtained for this uncommitted candidate. The workflow push trigger now
  includes `compiler/predication-v0`; run IDs must be recorded after pushing a
  final commit.
- Post-merge `main` checks remain pending. The PR remains draft because the
  recurrence-driven per-iteration predicated Store hardware gate is still
  unresolved.

## Release decision

**STOP — remaining T017 blocker.** Do not begin T018 until the predicated Store
Golden/RTL path, hosted gates, and a fresh source audit are green.
