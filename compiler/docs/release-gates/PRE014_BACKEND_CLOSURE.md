# PRE014 Backend Closure Release Gate

This record is the checklist for the backend gate that precedes T-COMP-014.
It is intentionally evidence-based: a workflow file is not a passing run.

## Required gates

| Gate | Command / evidence | Result |
| --- | --- | --- |
| Compiler build and semantic tests | `ctest --test-dir compiler/build/dev-debug --output-on-failure` | PASS (17/17) |
| Compiler sanitizer lane | `ctest --test-dir compiler/build/ci-sanitize --output-on-failure` | PASS (20/20) |
| Generated-program hardware E2E | `make compiler-e2e` | PASS (compiler, manifest validator, Verilator, golden/RTL, observations) |
| Retained hardware regression | `make regression` | PASS |
| Tiny independent oracle | `cgra-modulo-mapper-tests` | PASS (feasible/infeasible and mapper completion tests) |

## Audited evidence

- Source commit: `223b78ac51f0518cb54eea5e1fc3e6094554694f`
- Generated-program artifact: `build/compiler-e2e/fixed_addr_load_add_store/e2e_result.json`
- Primary E2E starts at `MII=1`, maps at `II=1`, and records 18 complete
  candidates with 17 same-II RF rejections before acceptance.
- E2E manifest hash matched the archived replay manifest; Verilator reported
  `PROGRAM_RTL_PASS`, the retained comparator reported `PROGRAM_TRACE_MATCH`,
  and the independent observation checker reported 4 expected stores in both
  golden and RTL traces.

## Closure assertions

- The production compiler starts at the analyzer MII; the primary E2E has no
  fixture-specific `minII` or virtual-hold disable.
- Complete T005 mappings are passed through an opaque completion checker.
  Stage/RF semantic infeasibility rejects the candidate at the same II;
  budget and verifier failures abort the search.
- RF allocations contain exact physical read/write ports. Lowering consumes
  those assignments and does not infer ports from operand indices.
- Constants are compactly allocated into target constant memory. Semantic IDs
  are not physical addresses.
- `make compiler-e2e` uses the generated manifest and checks compiler,
  manifest, golden/RTL, and handwritten semantic observation oracles.

## Limitations that do not block T014

General host ABI, LLVM input, MVE/rotating registers, spills, multicast, and
symbolic runtime trip counts remain future work. They are not substitutes for
this gate's backend evidence.
