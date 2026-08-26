# T017 Predication V0 Release Gate

Status: functional closure complete; hosted exact-head, PR merge-ref, and
post-merge `main` evidence pending.

Base `main` after T015/T016 evidence hardening:
`4679644c5ef79a16920eea15ac4c73e7f628f304`

Feature branch: `compiler/predication-v0`

Closure implementation SHA:
`2ec558243e94dea615fbc046e5198690febcc77a`

## Frozen V0 Scope

The LLVM frontend accepts one structured diamond or triangle, or a direct
LLVM `select`. It lowers an LLVM `icmp` to a Generic predicate, merge PHIs to
Generic `Select`, and one direct-address conditional Store to a Generic Store
with a predicate operand and a distance-one self-WAW edge. Pure arm arithmetic
is speculated; branch-local Load, GEP Store, multiple Stores, unsafe side
effects, nested branches, and conditional recurrence remain structured
failures.

The recurrence-driven Store fixture uses the existing T016 recurrence,
predicate routing, T014 invocation binding, production mapper, stage/RF
allocation, target lowering, generated manifest, Golden model, and Verilator.
It does not use an ABI-only route, register preload, fixed placement, or a
patched manifest.

## Blocker Classification And Repair

The original one-node recurrence fixture exposed two independent facts:

1. Its direct fixed-register lifetime has a genuine next-version overlap on
   the current non-rotating RF model. This is retained as the generic
   `RFA_FIXED_REGISTER_SELF_OVERLAP` regression; it is not weakened or reported
   as mapper infeasibility.
2. A backend-feasible equivalent two-node periodic recurrence exposed a
   generic TargetLowering defect: RF-backed NodeIssue operands were resolved
   using a default tile before the mapped placement was applied. TargetLowering
   now resolves placement first. A non-LLVM production-pipeline regression
   verifies the resulting manifest with the independent schedule checker.

The mapper also preserves per-II budget classification and retries feasible
higher IIs without converting `BudgetExceeded` into `Infeasible`. No fixture,
function, PE, slot, route, RF, port, or fixed-II special case was added.

## Independent Verification

`LLVMFrontendVerifier` independently reconstructs the internal branch and
merge, checks compare polarity and operands, ordinary SSA/external/constant
providers, recurrence edges, Select completeness and uniqueness, Store
address/data/predicate providers, self-WAW distance, LiveOut completeness, and
silent instruction loss.

Corruption tests reject, among others:

- omitted and duplicate merge Selects;
- swapped Select arms and wrong ordinary operand providers;
- wrong recurrence distance;
- missing or wrong Store predicate;
- missing or wrong-distance Store self-WAW;
- missing LiveOut and semantic instruction loss.

The existing negative corpus retains structured rejection for unsafe
speculation, predicated Load, multiple Store, GEP Store, nested/multiple branch,
and unsupported conditional recurrence forms.

## Local Evidence

- Fresh Debug CTest: PASS, 20/20.
- Fresh sanitizer unit/semantic CTest: PASS, 18/18.
- `make llvm-recurrence-e2e`: PASS, including external seed, scalar induction,
  induction poison, and trip counts 1/4/7.
- `make llvm-predication-e2e`: PASS for both value-merge polarities and both
  recurrence-driven Store cases.
- Value merge: compiler-generated manifest, Golden/RTL equality, layout-aware
  ABI output observation PASS.
- Store `tripCount=4, limit=2`: four issues, two commits, committed data
  sequence `[0, 1]`, Golden/RTL equality PASS.
- Store `tripCount=4, limit=0`: four issues, zero commits, Golden/RTL equality
  PASS.
- The lowering regression also passes with an artifact directory containing
  spaces, proving its independent checker invocation does not depend on an
  unquoted path.

## Hosted Evidence

The first exact closure candidate was `3eba4307d94c77f9dbcbd6b81592cca83f73035b`.
Its feature-head gates passed:

- exact feature-head compiler-fast: [run 32986749691](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32986749691) — PASS.
- exact feature-head hardware-regression: [run 32986749710](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32986749710) — PASS, including T013/T014/T015/T016 and T017 predicated-Store E2E.

PR [#5](https://github.com/shiyunyao/predicated-mesh-cgra/pull/5) merge-ref
for the same source head also passed:

- PR merge-ref compiler-fast: [run 32986742094](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32986742094) — PASS.
- PR merge-ref hardware-regression: [run 32986742093](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32986742093) — PASS, including the full backend and T017 predicated-Store E2E.

This documentation update will be pushed as a new feature head and its
exact-head gates will be recorded here as well. The final merge and main runs
remain pending:

- final feature-head compiler-fast: PENDING
- final feature-head hardware-regression: PENDING
- merged-main compiler-fast: PENDING
- merged-main hardware-regression: PENDING

The hardware workflow must continue to execute retained RTL/shared-scratchpad/
modulo-loop regressions and T013, T014, T015, T016, T017 value-merge, and T017
predicated-Store E2E targets.

## Release Decision

**STOP pending final documentation-head and post-merge gates.** Functional T017
closure and the first exact-head/merge-ref gates are green. T018 must not begin
until this documentation update's exact feature-head gates pass, PR #5 is
merged, post-merge `main` gates pass, and this record is sealed with those
immutable SHAs and run URLs.
