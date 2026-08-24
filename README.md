# Compiler-Controlled Predicated Mesh CGRA

A compact SystemVerilog implementation of a statically scheduled CGRA, with
Verilator tests covering the main datapath, routing, memory, and array-level
behavior.

The RTL implements a compiler-controlled 2D mesh. Computation and communication
are scheduled ahead of time and encoded in per-tile control memories; there is
no runtime arbitration or dynamic routing.

## Architecture

The default configuration is a 4x4 mesh. All tiles share a global `kernel_pc`,
while each tile reads its own control word for the current cycle.

A tile contains:

* a functional unit
* data and predicate register files
* control and constant memories
* data and predicate switchboxes
* an optional load/store unit

Tiles can execute an operation and drive routed outputs in the same cycle.

North, south, east, and west links are registered. Crossing one tile boundary
therefore takes one cycle, and multi-hop communication has to be represented
explicitly in the schedule.

Predicate values use a separate one-bit register file and routing network.
Comparisons and predicate operations can generate predicates, `select` can
choose between data values, and stores may be predicated.

LSUs are enabled with `HAS_LSU_MASK`. In the default 4x4 setup, the first tile
of each row has an LSU. Enabled LSUs receive dedicated ports of one array-level
shared scratchpad in deterministic row-major order: `(0,0)` through `(3,0)` map
to ports 0 through 3. Every enabled LSU may access every valid scratchpad
address; there is no runtime arbitration, stall, retry, or bank ownership.

Scratchpad addresses are 32-bit word addresses. Loads have a fixed two-cycle
architectural return latency. Multiple ports may load the same address in one
cycle, but a same-cycle same-address access involving a store is illegal and is
checked by RTL simulation, the golden model, and schedule tooling.

| Default resource |                      Size |
| ---------------- | ------------------------: |
| Array            |               4 x 4 tiles |
| Data path        |                   32 bits |
| Data RF          |       16 entries per tile |
| Predicate RF     |       16 entries per tile |
| Constant memory  |       16 entries per tile |
| Control memory   |      256 entries per tile |
| Shared scratchpad | 4096 x 32-bit words, 4 ports |

The FU supports pass-through, arithmetic, bitwise, shift, select, comparison,
and predicate operations. The compiler-facing opcode, source-selection, and
control-word encodings are defined in `target/cgra_v2.json`; RTL and Python
implementations are checked against that contract by regression tests.

## Repository Layout

```text
.
|-- rtl/       # SystemVerilog RTL
|-- tb/        # Unit and integration testbenches
|-- sim/       # Verilator C++ harnesses
|-- examples/  # Pre-generated target-encoded schedules
|-- model/     # Cycle-level golden execution model
|-- target/    # Compiler-facing machine-readable target contract
|-- compiler/include/cgra/IR/ # Target-independent generic loop DFG
|-- tools/     # Manifest validation, replay, config emission, and comparison
|-- scripts/   # Synthesis runners and report checks
|-- synth/     # RTL filelist and ASAP7/ABC inputs
|-- third_party/fncacti/ # Bundled FN-CACTI executable and upstream notes
|-- Makefile   # Build, test, lint, and synthesis targets
`-- .gitignore
```

The main hierarchy is:

```text
cgra_top
|-- loop controller
|-- per-tile control memories
|-- shared_scratchpad
`-- mesh
    `-- tile
        `-- LSU memory request unit (when enabled)
```

The LSU selects addresses, store data, and predicates and emits requests; it
does not own memory storage. The shared scratchpad is instantiated once by
`cgra_top` and is connected to the statically assigned LSU ports.

## Build and Test

Requirements:

* Verilator
* GNU Make
* Python 3
* a C++ compiler supported by Verilator

Run the default 4x4 test:

```bash
make test
```

A specific testbench can be selected with `TEST`:

```bash
make test TEST=fu_tb
make test TEST=mesh_2x2_tb
make test TEST=cgra_top_tb
```

Run all retained tests:

```bash
make regression
```

Lint a selected hierarchy:

```bash
make lint TEST=cgra_top_tb
```

The test suite includes unit tests for the FU, register files, source muxes,
routing, control-word packing, and LSU behavior, together with tile, mesh, and
full-array integration tests.

## Compiler Target Contract

`target/cgra_v2.json` is the compiler-facing source of truth for target
capabilities, resource limits, timing semantics, numeric encodings, and the
126-bit semantic control layout (stored as four 32-bit chunks). The C++ target
library does not parse RTL or Python files and does not change the existing
`cgra.program_manifest.v1` handoff.

The C++ contract build uses the CMake package `nlohmann_json` version 3.10.5
(install `nlohmann-json3-dev` on Ubuntu, or enable the pinned FetchContent
dependencies in the CI preset).

Build and test the standalone C++20 contract library without LLVM:

```bash
cmake -S compiler -B compiler/build
cmake --build compiler/build
ctest --test-dir compiler/build --output-on-failure
```

Inspect a loaded target contract with:

```bash
compiler/build/bin/cgra-target-dump target/cgra_v2.json
```

The legacy `enums` block remains for older tools as a compatibility view only;
the numeric `encodings` block and `control_layout` are authoritative for C++.

The contract tests validate malformed-target diagnostics, architectural
resource constants, every packed control encoding, known control images, and
exact decode-to-encode round trips for the retained shared-memory schedule.

Compiler CI conventions, deterministic test seeds, failure reproducer artifacts,
metrics schemas, and the local equivalents of both GitHub Actions workflows are
documented in [compiler/docs/CI_CONTRACT.md](compiler/docs/CI_CONTRACT.md).

The target-independent loop DFG is documented in
[compiler/docs/GENERIC_DFG_IR.md](compiler/docs/GENERIC_DFG_IR.md). Its CTest
suite covers stable IDs, cyclic recurrences, predicate and memory dependences,
canonical fixtures, and deterministic JSON round trips.

Generic DFG validity is checked independently by
[compiler/docs/GENERIC_DFG_VERIFIER.md](compiler/docs/GENERIC_DFG_VERIFIER.md);
the verifier has no target or LLVM dependency and emits stable diagnostic codes
and `cgra.dfg.verification.v1` JSON reports.

Target legalization is the next explicit compiler boundary. It maps a verified
Generic DFG one-to-one onto target-semantic operations, attaches target latency
and issue occupancy from `TargetModel`, preserves dependence distances and
provenance, and leaves placement, routing, registers, and control encoding to
later passes. See [compiler/docs/TARGET_LEGALIZATION.md](compiler/docs/TARGET_LEGALIZATION.md).

The mapper foundation is the finite modulo resource model and stage-free
`ModuloMapping` IR. It replicates FU, LSU, and directional Data/Predicate link
resources over exactly `[0, II)`, uses relative transport elapsed time and
explicit `VirtualHold` storage events, and does not perform placement or route
search. See [compiler/docs/MODULO_RESOURCE_MODEL.md](compiler/docs/MODULO_RESOURCE_MODEL.md).

The standalone legalizer CLI is built with the compiler targets:

```bash
compiler/build/bin/cgra-target-legalize \
  input.dfg.json --target target/cgra_v2.json \
  -o output.target_dfg.json --dump-text
```

## External Program Replay

The compiler/framework handoff is a scheduled and target-encoded
`cgra.program_manifest.v1` JSON file. The external compiler owns placement,
scheduling, register allocation, routing, and control-word encoding. The RTL
framework validates the supplied image without rescheduling it, emits the
configuration writes and protocol testbench, runs the golden model and
Verilator, and compares both cycle-level traces field by field.

`examples/schedules/shared_memory_cross_lsu_4x4.json` is the default already
scheduled and target-encoded 4x4 program manifest. It preloads global memory
from a non-LSU tile image, performs a store through port 0, issues four
concurrent loads, and then has every LSU load the value written by tile `(0,0)`.
The example therefore exercises global preload semantics, cross-LSU memory
visibility, all four static ports, legal same-address loads, and the fixed
two-cycle load response in the complete golden-versus-RTL flow.

Replay the default manifest through the complete flow:

```bash
make program
```

Pass another compiler output with `PROGRAM_MANIFEST`:

```bash
make program PROGRAM_MANIFEST=path/to/program_manifest.json
```

The individual `program-prepare`, `program-build`, `program-run`, and
`program-check` targets expose each stage. For a standalone one-command runner,
use:

```bash
python3 tools/program_runner.py \
  --run path/to/program_manifest.json \
  --out-dir build/program/my_program
```

A successful run ends with `PROGRAM_TRACE_MATCH`. The archived manifest,
configuration stream, generated SystemVerilog testbench, golden and RTL CSV
traces, and logs are written below `build/program/`; `artifacts.json` records
the input and preparation-artifact hashes.

The older
`examples/schedules/fir32_transposed_predicated_ii7_4x4.semantic.json` remains
as a legacy private-bank target artifact and is not silently reinterpreted as a
shared-memory schedule. A compiler targeting the current architecture must
emit `target/cgra_v2.json` and one global word-addressed scratchpad image.

Use:

```bash
make help
```

to list available testbenches, and:

```bash
make clean
```

to remove generated files. Build artifacts are written under `build/`.

## Synthesis

The current area breakdown is:

| Area component | Model basis | 2 x 2 (um^2) | 4 x 4 (um^2) |
| --- | --- | ---: | ---: |
| Mapped logic | ASAP7 7 nm, memory excluded | 3183.99498 | 11751.30504 |
| Control memories | FN-CACTI | 13347.760 | 53391.040 |
| Data register files | FN-CACTI | 1341.096 | 5364.384 |
| Predicate register files | FN-CACTI | 751.220 | 3004.880 |
| Constant memories | FN-CACTI | 486.152 | 1944.608 |
| Shared 4096 x 32, 4RW scratchpad | FN-CACTI, one instance | 24766.500 | 24766.500 |
| FN-CACTI storage subtotal | 14 nm devices, 7 nm wires | 40692.728 | 88471.412 |
| Arithmetic breakdown total | Mixed-node proxy | 43876.72298 | 100222.71704 |

FN-CACTI supports 14 nm FinFET devices and 7 nm ASAP7 wires in this flow; it
cannot produce a 7 nm device model. The arithmetic total above is therefore a
paper-style component sum of ASAP7 7 nm logic and a 14 nm-device/7 nm-wire
storage proxy. It is not a node-normalized 7 nm physical area, and no area
scaling has been applied.

The remaining timing and power results are:

| Result | 2 x 2 | 4 x 4 |
| --- | ---: | ---: |
| Combinational delay | 9599.95 ps (memory excluded) | 9516.86 ps (memory excluded) |
| Margin at 100 MHz | 400.05 ps (memory excluded) | 483.14 ps (memory excluded) |
| ABC total power | 1.60225e+06 raw units (memory excluded) | 6.13667e+06 raw units (memory excluded) |
| FN-CACTI storage leakage | 0.444880508 mW | 0.955536032 mW |
| FN-CACTI bounded-access storage power at 100 MHz | 1.561019228 mW | 4.109654912 mW |

The ABC rows use internally generated switching frames rather than the captured
RTL SAIF, so they are uncalibrated feasibility values rather than workload
power. The FN-CACTI rows are analytical storage-only scenarios and are not
added to ABC because the results do not share a compatible SI-unit and activity
basis.

The synthesis flow requires Python 3, oss-cad-suite Yosys with the
`yosys-slang` plugin, Verilator, `yosys-abc`, and a 7z-compatible extractor for
the first ASAP7 download. The bundled FN-CACTI executable also requires an x86
Linux environment capable of running its 32-bit binary. Source the tool
environment and run the desired target:

```bash
source ../oss-cad-suite/environment
make synth-area
make synth-timing
make synth-power
make synth-fn-cacti
```

Set `ASAP7_7Z=/absolute/path/to/7z` when the extractor is not available as
`7z` on `PATH`. Summaries and raw synthesis artifacts are generated under
`reports/synthesis/` and `sim/synthesis/`. `make synth-fn-cacti` models the
control memories, data and predicate register files, constant memories, and
one shared 4096-word scratchpad with four read/write ports. It reruns the
logic-area measurement, creates canonical and replay evidence under
`reports/synthesis/fn_cacti/`, and validates both runs.

## Configuration and Execution

`cgra_top` is configured through a ready/valid write interface. Each write
selects an address and a memory type; tile coordinates select local control and
constant memories but do not select scratchpad ownership:

| `cfg_mem_type` | Destination       |
| -------------: | ----------------- |
|            `0` | Control memory    |
|            `1` | Constant memory   |
|            `2` | Shared scratchpad |
|            `3` | Loop descriptor (global tile only) |

Control words wider than the configuration datapath are written in 32-bit
chunks using `cfg_word_idx`. Shared scratchpad configuration writes are global,
use tile `(0,0)` and `cfg_word_idx = 0`, and interpret `cfg_addr` as a word
address. Preload entries from all manifest tile images are gathered into one
global address space and duplicate global addresses are rejected.

Once configuration is complete, set `run_cycles` to the schedule length and
pulse `start`. The execution state is available through `busy`, `done`, and
`kernel_pc`.

## Modulo Loop Replay

The external compiler can describe a modulo schedule in the same
`cgra.program_manifest.v1` handoff by adding a `loop` object with
`prologue_cycles`, `ii`, `trip_count`, and `epilogue_cycles`. The framework
does not schedule or transform this image: it validates the descriptor, emits
the bounded prologue/kernel/epilogue control image (`prologue + ii + epilogue`
physical control slots), programs the RTL loop descriptor, and checks the
expanded cycle-by-cycle trace. The loop descriptor is committed through the
global tile and is then reused for each kernel iteration.

Replay the checked-in modulo-scheduling example, including trip-count,
zero-boundary, descriptor-reuse, and invalid-descriptor checks:

```bash
make modulo-loop
```

The example is `examples/schedules/modulo_mesh_feedback.json` (prologue 2,
II 2, trip count 4, epilogue 1, for 11 execution cycles). To exercise another
compiler-generated manifest through the same path, set
`MODULO_LOOP_PROGRAM`:

```bash
make modulo-loop MODULO_LOOP_PROGRAM=path/to/loop_manifest.json
```

The standalone preparation and comparison stages are also available through
`tools/modulo_loop_runner.py`; generated configurations, testbenches, models,
traces, and logs are kept under `build/modulo_loop/`.

`ROWS`, `COLS`, and `HAS_LSU_MASK` are top-level parameters. `cgra_top` also
exports the directional data outputs of every tile, including their
write-enable signals.

The semantic control word remains 126 bits and its physical configuration
representation remains four 32-bit chunks (128 bits). Shared memory adds no
bank ID or port ID field to that compiler contract.

## Tracing

Two integration tests enable cycle-level CSV tracing:

```bash
make test TEST=trace_tb
make test TEST=trace_extended_tb
```

The generated traces are written to:

```text
build/trace_tb/trace.csv
build/trace_extended_tb/trace.csv
```
