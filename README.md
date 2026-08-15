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
|-- scripts/   # Synthesis runners and report checks
|-- synth/     # RTL filelist and ASAP7/ABC inputs
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

The current ASAP7 RVT/TT synthesis results are:

| Result | 2 x 2 | 4 x 4 |
| --- | ---: | ---: |
| Mapped cell area | 2922.1965 (memory excluded) | 11507.60034 (memory excluded) |
| Combinational delay | 9642.85 ps (memory excluded) | 9553.48 ps (memory excluded) |
| Margin at 100 MHz | 357.15 ps (memory excluded) | 446.52 ps (memory excluded) |
| ABC total power | 1.52088e+06 raw units (memory excluded) | not run |

The synthesis flow requires Python 3, oss-cad-suite Yosys with the
`yosys-slang` plugin, Verilator, `yosys-abc`, and a 7z-compatible extractor for
the first ASAP7 download. Source the tool environment and run the desired
target:

```bash
source ../oss-cad-suite/environment
make synth-area
make synth-timing
make synth-power
```

Set `ASAP7_7Z=/absolute/path/to/7z` when the extractor is not available as
`7z` on `PATH`. Summaries and raw synthesis artifacts are generated under
`reports/synthesis/` and `sim/synthesis/`.

## Configuration and Execution

`cgra_top` is configured through a ready/valid write interface. Each write
selects a tile, an address, and one of the local memories:

| `cfg_mem_type` | Destination       |
| -------------: | ----------------- |
|            `0` | Control memory    |
|            `1` | Constant memory   |
|            `2` | Scratchpad memory |

Control words wider than the configuration datapath are written in 32-bit
chunks using `cfg_word_idx`.

Once configuration is complete, set `run_cycles` to the schedule length and
pulse `start`. The execution state is available through `busy`, `done`, and
`kernel_pc`.

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
