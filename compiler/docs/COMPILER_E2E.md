# Compiler-generated program E2E

The first whole-compiler integration fixture starts at a checked-in Generic DFG and
ends at the retained golden/RTL replay flow. Run it from the repository root with:

```sh
make compiler-e2e
```

The fixture is `fixed_addr_load_add_store`. It preloads shared scratchpad words 0 and
1 with 7 and 11, then observes four committed stores of 18 at word 2. The expected
observations are handwritten in
`compiler/tests/e2e/fixtures/fixed_addr_load_add_store/expected_observations.json`;
the observation checker does not participate in compilation or golden-model
generation.

The compiler driver uses the production verifier pipeline and writes numbered stage
artifacts under `build/compiler-e2e/fixed_addr_load_add_store/compiler`. The generated
manifest is then passed to `make program`, which prepares the retained configuration
stream, runs Verilator, generates the golden trace, and compares golden and RTL
traces. The final checker reads both traces and validates the expected committed
stores. `e2e_result.json` records the compiler/replay statuses and SHA-256 hashes.

The v1 loop descriptor repeats the compact kernel image. When T011 has already placed
boundary instances in the explicit prologue or epilogue, its kernel repeat count can
differ from the source trip count. The compiler pipeline report remains the source of
the finite trip-count and phase statistics; the manifest follows the existing v1
replay contract.

To stop after manifest generation, invoke the compiler executable directly:

```sh
build/compiler/bin/cgrac-compile-dfg \
  compiler/tests/e2e/fixtures/fixed_addr_load_add_store/generic_dfg.json \
  --target target/cgra_v3.json --trip-count 4 --max-ii 8 \
  --scratchpad-preload \
  compiler/tests/e2e/fixtures/fixed_addr_load_add_store/scratchpad_preload.json \
  --artifact-dir build/compiler-e2e/debug/compiler \
  -o build/compiler-e2e/debug/program_manifest.json
```

All artifacts are deterministic for fixed options. The primary functional test does
not snapshot PE/slot choices or control chunks; those remain replaceable mapper and
lowering details. A separate compiler-only CTest repeats the pipeline in two clean
temporary directories and requires byte-identical generated manifests.
