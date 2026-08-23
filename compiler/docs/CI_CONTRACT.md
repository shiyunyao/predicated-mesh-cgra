# Compiler CI Contract

This document freezes the safety boundary for compiler development. Compiler
algorithms may change their search order, heuristics, and internal data
structures as long as semantic verifiers, deterministic replay, and explicit
resource limits remain intact.

## Local Commands

The compiler uses C++20, CMake, Ninja, CTest, and a pinned GoogleTest smoke
lane. The presets enable the pinned dependencies through
`CGRA_FETCH_DEPS=ON`; an offline fallback is documented below.

The presets fetch exact dependency releases through CMake FetchContent. In an
offline environment, use `-DCGRA_FETCH_DEPS=OFF` and install the exact CMake
package `nlohmann_json` 3.10.5 (on Ubuntu this is typically provided by
`nlohmann-json3-dev`).

Developer build:

```bash
cd compiler
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug --output-on-failure
```

CI-equivalent debug build:

```bash
cmake --preset ci-debug
cmake --build --preset ci-debug
ctest --preset ci-debug --output-on-failure
```

Sanitizer lane (compiler unit and semantic tests only):

```bash
cmake --preset ci-sanitize
cmake --build --preset ci-sanitize
ctest --preset ci-sanitize --output-on-failure
```

The compiler workflows use `ccache` where available. Build trees live below
`compiler/build/`, which is ignored by Git.

Hardware/backend verification remains owned by the repository Makefile. CI
calls these same commands locally available to developers:

```bash
make regression
make shared-scratchpad-negative-tests
make program PROGRAM_MANIFEST=examples/schedules/shared_memory_cross_lsu_4x4.json
make modulo-loop
```

## Test Categories

`unit` tests cover small helper behavior and serialization primitives with a
10-second timeout. `semantic` tests cover cross-component architectural
invariants with a 30-second timeout. `e2e` is reserved for a future complete
compiler-to-backend pipeline and has a 120-second timeout.

CTest labels and timeouts are assigned by `compiler/cmake/Testing.cmake`; test
registration must use `cgra_add_test` rather than duplicating those properties.

## Determinism

Compiler tests use `CGRA_TEST_SEED=0` by default. `cgra::test::getTestSeed()`
accepts an explicit seed first, then reads that environment variable, and
falls back to zero. Future randomized compiler tools must expose
`--seed <uint64>` and record the seed in their metrics and failure artifacts.
Implicit `std::random_device` seeding is prohibited.

## Failure Reproducers

Test-only artifact helpers write under `CGRA_TEST_ARTIFACT_DIR`, or a
`build/failures/` directory below the current working directory when unset. A
nontrivial compiler failure should be
self-contained and avoid absolute machine paths.

The reserved failure schema is `cgra.compiler.failure.v1`:

```json
{
  "schema": "cgra.compiler.failure.v1",
  "test": "two_hop_recurrence",
  "component": "modulo_mapper",
  "reason": "budget_exceeded",
  "seed": 0,
  "status": "failure"
}
```

Once a mapper exists, a failure directory must contain at least:
`failure.json`, `command.txt`, `seed.txt`, `target.json`, and the input file.
Optional `config.json`, `partial_result.*`, `metrics.json`, and `notes.txt`
are encouraged.

## Metrics

The reserved metrics schema is `cgra.compiler.metrics.v1`:

```json
{
  "schema": "cgra.compiler.metrics.v1",
  "case": "placeholder",
  "component": "target_model",
  "status": "success",
  "seed": 0,
  "wall_time_ms": 1.23,
  "metrics": {}
}
```

Allowed statuses are `success`, `infeasible`, `budget_exceeded`,
`invalid_input`, and `internal_error`. II, wall time, node attempts, route
expansions, backtracks, and resource usage are record-only at this stage; no
quality threshold gates merges yet.

Future search implementations must expose explicit node-attempt, route-
expansion, and backtrack budgets. `infeasible` means the bounded search proved
there is no solution; `budget_exceeded` means the configured budget ended the
search before that proof. An external process timeout is a hard CI failure,
not a substitute for either status.

## Verification Boundary

Producers do not define their own correctness. Future mapper output must be
checked by an independent mapping verifier, then lowered through the existing
manifest validator, golden model, and RTL trace comparison. Tests should assert
legality, dependency preservation, resource non-conflict, deterministic replay,
and functional correctness rather than heuristic-specific tile or route
snapshots. Exact snapshots remain appropriate for target encoding and other
explicitly deterministic low-level primitives.
