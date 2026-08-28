# T021-T025 Local Mapper Coverage Expansion

This local-only checkpoint records the implementation and evidence for the
recurrence-ingress, finite-feasibility search, admissibility reporting, and
audit comparison work in this worktree.

## Scope

- T021 adds opt-in recurrence ingress normalization for non-memory loop-carried
  data and predicate edges. The original distance and boundary remain on the
  producer-to-ingress edge; the ingress-to-consumer edge has distance zero.
- T022 adds an explicit `find-any-feasible` mapper objective and an extended-II
  fallback search. Every candidate still goes through the complete stage/RF
  checker. The current fallback is an extended deterministic search lane; it is
  not a separate absolute-time constructive scheduler.
- T025 adds strict terminal status mapping, per-case comparison, and coverage
  metrics. Raw route mappings are never reported as strict feasible mappings.
- T023 address and conservative-memory support already present in the frontend
  remains target-independent and is covered by the retained memory tests.
- T024 remains limited to the existing structured if-conversion contract; a
  general multi-branch Predicate-SSA implementation is not claimed here.

## Local Evidence

All commands were run in the local worktree. No pull, fetch, push, remote
branch, pull request, tag, release, or GitHub write operation was performed.
The authoritative measurements are in
`artifacts/local-mapper-coverage/final/LOCAL_MAPPER_COVERAGE_IMPACT_REPORT.md`.

The full corpus attempt is retained separately from the reproducible six-case
smoke audit. The local host lacks the 32-bit libc development headers required
by several `-m32` sources, and the full run reached a long RF/mapping search;
these are reported as blockers rather than converted into coverage.

## Verification Contract

Strict success requires Generic/Target DFG verification, modulo mapping
verification, stage assignment verification, finite RF allocation and RF
verification. The complete mapping checker remains enabled for both hardware
and mapping-research modes. `FEASIBLE_II` therefore means a finite-RF result;
`ROUTE_MAPPED` is only a raw observation.

## Release Decision

This is a local implementation checkpoint, not a hosted release seal. The
full-corpus and broad T021-T025 coverage gates remain open until a clean
toolchain run produces complete corpus accounting and the required strict
coverage numbers.
