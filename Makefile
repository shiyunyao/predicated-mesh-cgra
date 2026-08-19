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

MODULO_LOOP_PROGRAM ?= examples/schedules/modulo_mesh_feedback.json
MODULO_LOOP_DIR := $(BUILD_DIR)/modulo_loop
MODULO_LOOP_TB := $(MODULO_LOOP_DIR)/generated_program_tb.sv
MODULO_LOOP_CONFIG := $(MODULO_LOOP_DIR)/config_stream.json
MODULO_LOOP_GOLDEN := $(MODULO_LOOP_DIR)/golden_trace.csv
MODULO_LOOP_RTL := $(MODULO_LOOP_DIR)/rtl_trace.csv

ifeq ($(TEST),modulo_loop)
TB_SRC := $(MODULO_LOOP_TB)
TEST_BUILD_DIR := $(MODULO_LOOP_DIR)/obj
TRACE_FILE := $(MODULO_LOOP_RTL)
RUN_ARGS := +CGRA_TRACE +CGRA_LOOP_TRACE +CGRA_TRACE_FILE=$(TRACE_FILE)
endif

TOP_MODULE := $(if $(filter modulo_loop,$(TEST)),generated_program_tb,$(TEST))

PROGRAM_MANIFEST ?= examples/schedules/fir32_transposed_predicated_ii7_4x4.semantic.json
PROGRAM_NAME := $(basename $(notdir $(PROGRAM_MANIFEST)))
PROGRAM_DIR := $(BUILD_DIR)/program/$(PROGRAM_NAME)
PROGRAM_OBJ_DIR := $(PROGRAM_DIR)/obj
PROGRAM_TB := $(PROGRAM_DIR)/generated_program_tb.sv
PROGRAM_RTL_TRACE := $(PROGRAM_DIR)/rtl_trace.csv
PROGRAM_GOLDEN_TRACE := $(PROGRAM_DIR)/golden_trace.csv
PROGRAM_CONFIG := $(PROGRAM_DIR)/config_stream.json
PROGRAM_IMAGE := $(PROGRAM_DIR)/program_manifest.json
PROGRAM_CPP := $(abspath sim/data_rf_main.cpp)

.PHONY: help check-test lint build test regression program program-prepare program-build program-run program-check modulo-loop modulo-loop-prepare modulo-loop-check modulo-loop-tripcount-tests modulo-loop-zero-boundary-test modulo-loop-reuse-test modulo-loop-assert-tests synth-fetch-asap7 synth-memory-shape synth-area synth-timing synth-power synth-power-feasibility synth-fn-cacti clean

help:
	@echo "make test TEST=<name>  Build and run one testbench"
	@echo "make lint TEST=<name>  Lint RTL with one testbench"
	@echo "make regression         Run all retained testbenches"
	@echo "make program PROGRAM_MANIFEST=<path>  Replay an external cgra.program_manifest.v1 through RTL and compare traces"
	@echo "make modulo-loop        Replay the external modulo-loop manifest and run loop coverage"
	@echo "make synth-area         Map 2x2/4x4 logic and report ASAP7 cell area"
	@echo "make synth-timing       Estimate 2x2/4x4 logic timing at 100 MHz"
	@echo "make synth-power        Capture SAIF and probe 2x2/4x4 ABC power"
	@echo "make synth-fn-cacti     Model all architectural storage with FN-CACTI"
	@echo "Available tests: $(TESTS)"

check-test:
	@test -f "$(TB_SRC)" || { echo "Unknown test: $(TEST)"; exit 2; }

lint: check-test
	$(VERILATOR) --lint-only -Wall $(RTL_SRCS) $(TB_SRC) --top-module $(TEST)

build: check-test
	mkdir -p "$(TEST_BUILD_DIR)"
	$(VERILATOR) $(VERILATOR_FLAGS) --Mdir "$(TEST_BUILD_DIR)" --prefix Vtop \
		$(RTL_SRCS) $(TB_SRC) $(CPP_SRC) --top-module $(TOP_MODULE)

test: $(if $(filter modulo_loop,$(TEST)),modulo-loop-prepare) build
	"$(TEST_BUILD_DIR)/Vtop" $(RUN_ARGS)
	@if echo " $(TEST) " | grep -q " trace_"; then test -s "$(TRACE_FILE)"; fi
	@if [ "$(TEST)" = modulo_loop ]; then test -s "$(TRACE_FILE)"; fi

regression:
	@set -e; for test_name in $(TESTS); do \
		echo "==> $$test_name"; \
		$(MAKE) --no-print-directory test TEST=$$test_name; \
	done

program: program-check

program-prepare:
	@test -f "$(PROGRAM_MANIFEST)" || { echo "Missing program manifest: $(PROGRAM_MANIFEST)"; exit 2; }
	mkdir -p "$(PROGRAM_DIR)"
	python3 tools/program_runner.py --prepare "$(PROGRAM_MANIFEST)" --out-dir "$(PROGRAM_DIR)"

program-build: program-prepare
	mkdir -p "$(PROGRAM_OBJ_DIR)"
	$(VERILATOR) $(VERILATOR_FLAGS) --Mdir "$(PROGRAM_OBJ_DIR)" --prefix Vtop \
		-f synth/rtl_files.f "$(PROGRAM_TB)" "$(PROGRAM_CPP)" \
		--top-module generated_program_tb

program-run: program-build
	@loop_trace_arg=""; \
	if python3 -c 'import json, sys; m=json.load(open(sys.argv[1], encoding="utf-8")); raise SystemExit(0 if m.get("loop", {}).get("enabled") is True else 1)' "$(PROGRAM_IMAGE)"; then \
		loop_trace_arg=+CGRA_LOOP_TRACE; \
	fi; \
	"$(PROGRAM_OBJ_DIR)/Vtop" \
		+CGRA_TRACE +CGRA_TRACE_FILE="$(abspath $(PROGRAM_RTL_TRACE))" $$loop_trace_arg \
		> "$(PROGRAM_DIR)/rtl_simulation.log" 2>&1
	cat "$(PROGRAM_DIR)/rtl_simulation.log"
	@test -s "$(PROGRAM_RTL_TRACE)"

program-check: program-run
	python3 tools/program_runner.py --compare \
		--manifest "$(PROGRAM_IMAGE)" \
		--config "$(PROGRAM_CONFIG)" \
		--golden "$(PROGRAM_GOLDEN_TRACE)" \
		--rtl "$(PROGRAM_RTL_TRACE)" \
		> "$(PROGRAM_DIR)/compare.log" 2>&1
	cat "$(PROGRAM_DIR)/compare.log"

modulo-loop: modulo-loop-check modulo-loop-tripcount-tests modulo-loop-zero-boundary-test modulo-loop-reuse-test modulo-loop-assert-tests
	python3 -m pytest tests/test_modulo_loop.py

modulo-loop-prepare:
	mkdir -p "$(MODULO_LOOP_DIR)"
	python3 tools/modulo_loop_runner.py --prepare "$(MODULO_LOOP_PROGRAM)" --out-dir "$(MODULO_LOOP_DIR)"

modulo-loop-check:
	$(MAKE) --no-print-directory test TEST=modulo_loop
	python3 tools/modulo_loop_runner.py --compare --golden "$(MODULO_LOOP_GOLDEN)" --rtl "$(MODULO_LOOP_RTL)"

modulo-loop-tripcount-tests:
	$(MAKE) --no-print-directory test TEST=modulo_loop
	@set -e; for n in 1 2 7; do \
		out="$(MODULO_LOOP_DIR)/n$$n"; \
		mkdir -p "$$out"; \
		python3 tools/modulo_loop_runner.py --prepare "$(MODULO_LOOP_PROGRAM)" --trip-count $$n --out-dir "$$out"; \
		"$(MODULO_LOOP_DIR)/obj/Vtop" +LOOP_TRIP_COUNT_$$n +CGRA_TRACE +CGRA_LOOP_TRACE +CGRA_TRACE_FILE="$$out/rtl_trace.csv"; \
		python3 tools/modulo_loop_runner.py --compare --golden "$$out/golden_trace.csv" --rtl "$$out/rtl_trace.csv"; \
	done

modulo-loop-zero-boundary-test:
	$(MAKE) --no-print-directory test TEST=modulo_loop
	@set -e; out="$(MODULO_LOOP_DIR)/zero_boundaries"; \
	mkdir -p "$$out"; \
	python3 tools/modulo_loop_runner.py --prepare "$(MODULO_LOOP_PROGRAM)" --trip-count 1 --zero-boundaries --out-dir "$$out"; \
	"$(MODULO_LOOP_DIR)/obj/Vtop" +LOOP_TRIP_COUNT_1 +LOOP_ZERO_BOUNDARIES +CGRA_TRACE +CGRA_LOOP_TRACE +CGRA_TRACE_FILE="$$out/rtl_trace.csv"; \
	python3 tools/modulo_loop_runner.py --compare --golden "$$out/golden_trace.csv" --rtl "$$out/rtl_trace.csv"

modulo-loop-reuse-test:
	$(MAKE) --no-print-directory test TEST=modulo_loop_reuse_tb

modulo-loop-assert-tests:
	$(MAKE) --no-print-directory test TEST=modulo_loop
	@set -e; \
	"$(MODULO_LOOP_DIR)/obj/Vtop" +SKIP_LOOP_COMMIT +CGRA_TRACE +CGRA_LOOP_TRACE +CGRA_TRACE_FILE="$(MODULO_LOOP_DIR)/skip_commit_trace.csv"; \
	for arg in +INVALID_LOOP_II +INVALID_LOOP_SPAN +LOOP_DESC_DURING_RUN +LOOP_DESC_DURING_DONE; do \
		if "$(MODULO_LOOP_DIR)/obj/Vtop" $$arg > "$(MODULO_LOOP_DIR)/$${arg#+}.log" 2>&1; then \
			echo "expected failure did not occur for $$arg" >&2; exit 1; \
		fi; \
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

synth-fn-cacti: synth-fetch-asap7 synth-memory-shape
	python3 scripts/run_fn_cacti.py --target all
	python3 scripts/run_fn_cacti.py --target all --replay
	python3 scripts/check_fn_cacti.py reports/synthesis/fn_cacti/

clean:
	rm -rf -- "$(BUILD_DIR)"
