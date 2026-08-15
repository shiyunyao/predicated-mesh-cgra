# RTL synthesis estimates

This directory contains the minimum inputs needed to reproduce the existing
ASAP7 area, timing, and power-feasibility flows for `cgra_top`.

## Prerequisites

- oss-cad-suite Yosys with the `yosys-slang` plugin, Verilator, and `yosys-abc`;
- Python 3;
- a 7z-compatible extractor for the first ASAP7 download.

Source the tool environment before running synthesis. In the current workspace:

```bash
source ../oss-cad-suite/environment
```

The first run downloads the pinned ASAP7 v28 RVT/TT NLDM library into the
ignored `synth/asap7/v28/` cache. Set `ASAP7_7Z` to an absolute extractor path
when `7z` is not on `PATH`.

## Commands

```bash
make synth-area
make synth-timing
make synth-power
```

Area and timing run both the 2x2 debug target and default 4x4 target. Power uses
the deterministic 2x2 activity test and an independent ABC power probe. Stable
summaries are written to `reports/synthesis/`; raw scripts, logs, JSON, critical
paths, and SAIF files are written below `reports/synthesis/raw/` and
`sim/synthesis/`.

## Interpretation limits

- Area is mapped standard-cell area and excludes residual inferred memories.
- Timing is a 100 MHz, pre-layout ABC combinational estimate, not SDC STA or
  signoff timing closure.
- The RTL SAIF cannot be annotated into this toolchain's ABC flow. Reported
  power values use ABC internal switching frames and raw library/tool units;
  they are an uncalibrated feasibility result, not workload power.
- SRAM models, clock tree, placement, routing, parasitics, and variation are
  absent from all three flows.
