# SPDX-License-Identifier: MIT

VERILATOR ?= verilator
TEST ?= cgra_4x4_smoke_tb
BUILD_DIR ?= build
ASAP7_7Z ?= 7z

RTL_SRCS := \
	rtl/cgra_pkg.sv \
	rtl/data_rf.sv \
	rtl/pred_rf.sv \
	rtl/data_src_mux.sv \
	rtl/pred_src_mux.sv \
	rtl/fu.sv \
	rtl/data_switchbox.sv \
	rtl/pred_switchbox.sv \
	rtl/scratchpad_bank.sv \
	rtl/lsu.sv \
	rtl/tile.sv \
	rtl/mesh.sv \
	rtl/control_mem_bank.sv \
	rtl/cgra_top.sv

TESTS := \
	smoke_tb \
	cgra_pkg_tb \
	control_word_tb \
	data_rf_tb \
	pred_rf_tb \
	data_src_mux_tb \
	pred_src_mux_tb \
	data_switchbox_tb \
	pred_switchbox_tb \
	fu_tb \
	lsu_tb \
	tile_tb \
	tile_lsu_tb \
	mesh_link_tb \
	mesh_2x2_tb \
	cgra_top_tb \
	cgra_4x4_smoke_tb \
	runtime_corner_tb \
	random_dfg_tb \
	trace_tb \
	trace_extended_tb

CLOCKLESS_TESTS := smoke_tb cgra_pkg_tb control_word_tb
TB_SRC := tb/$(TEST).sv
TEST_BUILD_DIR := $(BUILD_DIR)/$(TEST)
HARNESS := sim/$(if $(filter $(TEST),$(CLOCKLESS_TESTS)),smoke_main.cpp,data_rf_main.cpp)
CPP_SRC := $(abspath $(HARNESS))
TRACE_FILE := $(TEST_BUILD_DIR)/trace.csv
RUN_ARGS := $(if $(filter $(TEST),trace_tb trace_extended_tb),+CGRA_TRACE +CGRA_TRACE_FILE=$(TRACE_FILE))
VERILATOR_FLAGS ?= -Wall --cc --exe --build

.PHONY: help check-test lint build test regression synth-fetch-asap7 synth-memory-shape synth-area synth-timing synth-power synth-power-feasibility clean

help:
	@echo "make test TEST=<name>  Build and run one testbench"
	@echo "make lint TEST=<name>  Lint RTL with one testbench"
	@echo "make regression         Run all retained testbenches"
	@echo "make synth-area         Map 2x2/4x4 logic and report ASAP7 cell area"
	@echo "make synth-timing       Estimate 2x2/4x4 logic timing at 100 MHz"
	@echo "make synth-power        Capture SAIF and run the ABC power probe"
	@echo "Available tests: $(TESTS)"

check-test:
	@test -f "$(TB_SRC)" || { echo "Unknown test: $(TEST)"; exit 2; }

lint: check-test
	$(VERILATOR) --lint-only -Wall $(RTL_SRCS) $(TB_SRC) --top-module $(TEST)

build: check-test
	mkdir -p "$(TEST_BUILD_DIR)"
	$(VERILATOR) $(VERILATOR_FLAGS) --Mdir "$(TEST_BUILD_DIR)" --prefix Vtop \
		$(RTL_SRCS) $(TB_SRC) $(CPP_SRC) --top-module $(TEST)

test: build
	"$(TEST_BUILD_DIR)/Vtop" $(RUN_ARGS)
	@if echo " $(TEST) " | grep -q " trace_"; then test -s "$(TRACE_FILE)"; fi

regression:
	@set -e; for test_name in $(TESTS); do \
		echo "==> $$test_name"; \
		$(MAKE) --no-print-directory test TEST=$$test_name; \
	done

synth-fetch-asap7:
	python3 scripts/fetch_asap7.py --seven-zip $(ASAP7_7Z)

synth-memory-shape:
	python3 scripts/check_control_memory_shape.py --target all

synth-area: synth-fetch-asap7 synth-memory-shape
	python3 scripts/run_synth_area.py --target all
	python3 scripts/summarize_synth_reports.py reports/synthesis/

synth-timing: synth-fetch-asap7 synth-memory-shape
	python3 scripts/run_synth_timing.py --target all
	python3 scripts/summarize_timing.py reports/synthesis/

synth-power: synth-fetch-asap7
	python3 scripts/run_power_feasibility.py --self-test
	python3 scripts/run_power_feasibility.py
	python3 scripts/check_power_feasibility.py --self-test
	python3 scripts/check_power_feasibility.py

synth-power-feasibility: synth-power

clean:
	rm -rf -- "$(BUILD_DIR)"
