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
|-- examples/  # Minimal DFGs and a pre-generated 4x4 schedule
|-- model/     # Cycle-level golden execution model
|-- target/    # Tooling description of the RTL target
|-- tools/     # DFG scheduling, config emission, and trace comparison
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

## Generated Program Replay

The retained compiler path accepts a small `cgra.dfg.v1` input, emits and
validates its scheduled program manifest and configuration stream, generates a
SystemVerilog testbench from those configuration writes, and runs that
testbench against the RTL. The resulting cycle-level RTL trace is compared
field by field with the golden-model trace.

Run the default single-tile add chain or the included 1x2 routing and predicate
example with:

```bash
make generated-program
make generated-program GENERATED_PROGRAM=examples/dfg/select_1x2.json
```

The retained examples have been verified against the unchanged 4x4 RTL:

| Input DFG | Active schedule | Trace records | Result |
| --- | --- | ---: | --- |
| `add_chain.json` | Single tile, 4 cycles | 64 | RTL/golden match |
| `select_1x2.json` | Two tiles, 6 cycles, one-hop route and predicate select | 96 | RTL/golden match |

A successful run ends with `GENERATED_PROGRAM_TRACE_MATCH`. Reproducible inputs,
the scheduled manifest, configuration stream, generated testbench, both CSV
traces, provenance hashes, and logs are written under
`build/generated_program/<dfg-name>/`.

This retained compiler is deliberately small: it supports constant-input,
topologically ordered, single-output programs on one tile or the included 1x2
one-hop data-transfer pattern. It is a reproducible schedule-to-RTL simulation
path, not a general CGRA mapper.

### Pre-Generated 4x4 Schedule Replay

`examples/schedules/fir32_transposed_predicated_ii7_4x4.semantic.json` is an
already scheduled and target-encoded 4x4 program manifest. It retains the
human-readable semantic controls and provides four 32-bit `chunks` for every
tile/PC control entry. This exercises the framework handoff expected from an
external compiler: the framework does not reschedule the program, but validates
the supplied schedule, emits its configuration stream and testbench, runs the
RTL, and checks the architectural stores.

Run the complete replay from the repository root:

```bash
SCHEDULE=examples/schedules/fir32_transposed_predicated_ii7_4x4.semantic.json
OUT=build/generated_schedule/fir32_transposed_predicated_ii7_4x4
OBJ="$OUT/obj"

mkdir -p "$OUT" "$OBJ"
python3 tools/validate_program.py "$SCHEDULE"
python3 tools/check_schedule.py "$SCHEDULE"
python3 tools/emit_config.py "$SCHEDULE" --out "$OUT/config_stream.json"

python3 - "$SCHEDULE" "$OUT" <<'PY'
import json
import pathlib
import sys

from model.golden_model import run_manifest, write_trace_csv
from tools.generated_program_runner import emit_testbench, rtl_compatible_trace_rows

schedule = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
config_path = out / "config_stream.json"
with config_path.open(encoding="utf-8") as handle:
    config = json.load(handle)

golden = rtl_compatible_trace_rows(run_manifest(schedule))
write_trace_csv(out / "golden_trace.csv", golden)
emit_testbench(config, schedule, config_path, out / "generated_program_tb.sv")
PY

verilator -Wall --cc --exe --build \
  --Mdir "$OBJ" --prefix Vtop \
  -f synth/rtl_files.f \
  "$OUT/generated_program_tb.sv" "$(pwd)/sim/data_rf_main.cpp" \
  --top-module generated_program_tb

"$OBJ/Vtop" \
  +CGRA_TRACE \
  +CGRA_TRACE_FILE="$(pwd)/$OUT/rtl_trace.csv"

python3 - "$SCHEDULE" "$OUT/rtl_trace.csv" <<'PY'
import csv
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    schedule = json.load(handle)
with open(sys.argv[2], newline="", encoding="utf-8") as handle:
    stores = [row for row in csv.DictReader(handle) if row["lsu_op"] == "2"]

expected = schedule["expected"]["outputs"]
assert len(stores) == len(expected) == 32
for want, got in zip(expected, stores):
    assert int(got["lsu_addr"], 16) == want["scratchpad_addr"]
    assert int(got["lsu_store_data"]) == int(want["y_internal"], 16)
    assert int(got["lsu_store_commit"]) == int(want["predicate"])

print("FIR_SCHEDULE_RTL_PASS stores=32 run_cycles=242")
PY
```

The retained result is a legal 242-cycle, II=7 schedule with 2,210 encoded
control entries and 8,969 configuration writes. RTL simulation produces 3,872
cycle/tile trace records; all 32 store addresses, data values, and predicate
commit decisions match the schedule's expected results.

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
