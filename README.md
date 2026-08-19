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
* a private scratchpad when the LSU is enabled

Tiles can execute an operation and drive routed outputs in the same cycle.

North, south, east, and west links are registered. Crossing one tile boundary
therefore takes one cycle, and multi-hop communication has to be represented
explicitly in the schedule.

Predicate values use a separate one-bit register file and routing network.
Comparisons and predicate operations can generate predicates, `select` can
choose between data values, and stores may be predicated.

LSUs are enabled with `HAS_LSU_MASK`. In the default 4x4 setup, the first tile
of each row has an LSU. Each LSU tile owns a separate scratchpad bank. Loads
have a fixed two-cycle return latency.

| Default resource |                      Size |
| ---------------- | ------------------------: |
| Array            |               4 x 4 tiles |
| Data path        |                   32 bits |
| Data RF          |       16 entries per tile |
| Predicate RF     |       16 entries per tile |
| Constant memory  |       16 entries per tile |
| Control memory   |      256 entries per tile |
| Scratchpad bank  | 1024 entries per LSU tile |

The FU supports pass-through, arithmetic, bitwise, shift, select, comparison,
and predicate operations. Opcode, source-selection, and control-word encodings
are defined in `rtl/cgra_pkg.sv`.

## Repository Layout

```text
.
|-- rtl/       # SystemVerilog RTL
|-- tb/        # Unit and integration testbenches
|-- sim/       # Verilator C++ harnesses
|-- examples/  # Pre-generated target-encoded schedules
|-- model/     # Cycle-level golden execution model
|-- target/    # Tooling description of the RTL target
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
`-- mesh
    `-- tile
```

The register files, FU, switchboxes, LSU, and scratchpad are kept in separate
RTL modules.

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

## External Program Replay

The compiler/framework handoff is a scheduled and target-encoded
`cgra.program_manifest.v1` JSON file. The external compiler owns placement,
scheduling, register allocation, routing, and control-word encoding. The RTL
framework validates the supplied image without rescheduling it, emits the
configuration writes and protocol testbench, runs the golden model and
Verilator, and compares both cycle-level traces field by field.

`examples/schedules/fir32_transposed_predicated_ii7_4x4.semantic.json` is an
already scheduled and target-encoded 4x4 program manifest. It retains the
human-readable semantic controls and provides four 32-bit `chunks` for every
tile/PC control entry. It is the retained example of the artifact an external
compiler must emit.

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

The retained FIR manifest is a legal 242-cycle, II=7 schedule with 2,210
encoded control entries. The replay loader materializes the complete 4x4
control image, filling omitted tile/PC entries with NOP writes. Its RTL
simulation produces 3,872 cycle/tile trace records.

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
| Mapped logic | ASAP7 7 nm, memory excluded | 2922.1965 | 11507.60034 |
| Control memories | FN-CACTI | 13347.760 | 53391.040 |
| Data register files | FN-CACTI | 1341.096 | 5364.384 |
| Predicate register files | FN-CACTI | 751.220 | 3004.880 |
| Constant memories | FN-CACTI | 486.152 | 1944.608 |
| Scratchpad banks | FN-CACTI | 8558.500 | 17117.000 |
| FN-CACTI storage subtotal | 14 nm devices, 7 nm wires | 24484.728 | 80821.912 |
| Arithmetic breakdown total | Mixed-node proxy | 27406.9245 | 92329.51234 |

FN-CACTI supports 14 nm FinFET devices and 7 nm ASAP7 wires in this flow; it
cannot produce a 7 nm device model. The arithmetic total above is therefore a
paper-style component sum of ASAP7 7 nm logic and a 14 nm-device/7 nm-wire
storage proxy. It is not a node-normalized 7 nm physical area, and no area
scaling has been applied.

The remaining timing and power results are:

| Result | 2 x 2 | 4 x 4 |
| --- | ---: | ---: |
| Combinational delay | 9642.85 ps (memory excluded) | 9553.48 ps (memory excluded) |
| Margin at 100 MHz | 357.15 ps (memory excluded) | 446.52 ps (memory excluded) |
| ABC total power | 1.52088e+06 raw units (memory excluded) | 6.03186e+06 raw units (memory excluded) |
| FN-CACTI storage leakage | 0.269045708 mW | 0.878528432 mW |
| FN-CACTI storage power, all declared ports active at 100 MHz | 1.141073828 mW | 3.981238112 mW |

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
scratchpad banks, reruns the logic-area measurement, creates canonical and
replay evidence under `reports/synthesis/fn_cacti/`, and validates both runs.

## Configuration and Execution

`cgra_top` is configured through a ready/valid write interface. Each write
selects a tile, an address, and one of the local memories:

| `cfg_mem_type` | Destination       |
| -------------: | ----------------- |
|            `0` | Control memory    |
|            `1` | Constant memory   |
|            `2` | Scratchpad memory |
|            `3` | Loop descriptor (global tile only) |

Control words wider than the configuration datapath are written in 32-bit
chunks using `cfg_word_idx`.

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
