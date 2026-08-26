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
	rtl/shared_scratchpad.sv \
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
	shared_scratchpad_tb \
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
VERILATOR_FLAGS ?= -Wall --Wno-fatal --cc --exe --build

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

PROGRAM_MANIFEST ?= examples/schedules/shared_memory_cross_lsu_4x4.json
PROGRAM_NAME := $(basename $(notdir $(PROGRAM_MANIFEST)))
PROGRAM_DIR := $(BUILD_DIR)/program/$(PROGRAM_NAME)
PROGRAM_OBJ_DIR := $(PROGRAM_DIR)/obj
PROGRAM_TB := $(PROGRAM_DIR)/generated_program_tb.sv
PROGRAM_RTL_TRACE := $(PROGRAM_DIR)/rtl_trace.csv
PROGRAM_GOLDEN_TRACE := $(PROGRAM_DIR)/golden_trace.csv
PROGRAM_CONFIG := $(PROGRAM_DIR)/config_stream.json
PROGRAM_IMAGE := $(PROGRAM_DIR)/program_manifest.json
PROGRAM_CPP := $(abspath sim/data_rf_main.cpp)

COMPILER_BUILD_DIR ?= build/compiler
COMPILER_E2E_DIR ?= build/compiler-e2e/fixed_addr_load_add_store
COMPILER_E2E_DFG := compiler/tests/e2e/fixtures/fixed_addr_load_add_store/generic_dfg.json
COMPILER_E2E_PRELOAD := compiler/tests/e2e/fixtures/fixed_addr_load_add_store/scratchpad_preload.json
COMPILER_E2E_EXPECTATIONS := compiler/tests/e2e/fixtures/fixed_addr_load_add_store/expected_observations.json
COMPILER_E2E_MANIFEST := $(COMPILER_E2E_DIR)/program_manifest.json
COMPILER_E2E_PROGRAM_DIR := $(COMPILER_E2E_DIR)
COMPILER_KERNEL_SCALAR_DIR ?= build/kernel-e2e/abi_scalar_input
COMPILER_KERNEL_BASE_DIR ?= build/kernel-e2e/abi_base_load
COMPILER_KERNEL_RECURRENCE_DIR ?= build/kernel-e2e/abi_recurrence_seed
COMPILER_KERNEL_TRIPCOUNT_DIR ?= build/kernel-e2e/abi_tripcount
LLVM_CONFIG ?= llvm-config-14
LLVM_DIR ?= $(shell $(LLVM_CONFIG) --cmakedir 2>/dev/null)
LLVM_FRONTEND_BUILD_DIR ?= build/compiler-llvm
LLVM_FRONTEND_E2E_DIR ?= build/llvm-frontend-e2e
LLVM_FRONTEND_CLANG ?= clang-14
LLVM_FRONTEND_OPT ?= opt-14
LLVM_CMAKE_ARG := $(if $(LLVM_DIR),-DLLVM_DIR=$(LLVM_DIR),)

.PHONY: help check-test lint build test regression shared-scratchpad-tests shared-scratchpad-negative-tests program program-prepare program-build program-run program-check compiler-e2e kernel-abi-e2e kernel-abi-scalar-e2e kernel-abi-base-load-e2e kernel-abi-recurrence-e2e kernel-abi-tripcount-e2e llvm-frontend-e2e llvm-frontend-c-smoke llvm-recurrence-e2e llvm-predication-e2e modulo-loop modulo-loop-prepare modulo-loop-check modulo-loop-tripcount-tests modulo-loop-zero-boundary-test modulo-loop-reuse-test modulo-loop-assert-tests synth-fetch-asap7 synth-memory-shape synth-area synth-timing synth-power synth-power-feasibility synth-fn-cacti clean

help:
	@echo "make test TEST=<name>  Build and run one testbench"
	@echo "make lint TEST=<name>  Lint RTL with one testbench"
	@echo "make regression         Run all retained testbenches"
	@echo "make program PROGRAM_MANIFEST=<path>  Replay an external cgra.program_manifest.v1 through RTL and compare traces"
	@echo "make kernel-abi-e2e  Compile scalar, base-address, recurrence, and trip-count ABI kernels through RTL"
	@echo "make llvm-frontend-e2e  Lower clang LLVM loops through Generic DFG, Kernel ABI, and RTL"
	@echo "make llvm-recurrence-e2e  Lower LLVM PHI recurrences through Generic DFG, Kernel ABI, and RTL"
	@echo "make llvm-predication-e2e  Lower LLVM if-conversion predicates through Generic DFG and verifier"
	@echo "make modulo-loop        Replay the external modulo-loop manifest and run loop coverage"
	@echo "make synth-area         Map 2x2/4x4 logic and report ASAP7 cell area"
	@echo "make synth-timing       Estimate 2x2/4x4 logic timing at 100 MHz"
	@echo "make synth-power        Capture SAIF and probe 2x2/4x4 ABC power"
	@echo "make synth-fn-cacti     Model all architectural storage with FN-CACTI"
	@echo "Available tests: $(TESTS)"

check-test:
	@test -f "$(TB_SRC)" || { echo "Unknown test: $(TEST)"; exit 2; }

lint: check-test
	$(VERILATOR) --lint-only -Wall --Wno-fatal $(RTL_SRCS) $(TB_SRC) --top-module $(TOP_MODULE)

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

shared-scratchpad-tests:
	$(MAKE) --no-print-directory test TEST=shared_scratchpad_tb
	$(MAKE) --no-print-directory shared-scratchpad-negative-tests

shared-scratchpad-negative-tests:
	$(MAKE) --no-print-directory build TEST=shared_scratchpad_tb
	@set +e; "build/shared_scratchpad_tb/Vtop" +CONFLICT_STORE_LOAD >/dev/null 2>&1; status=$$?; test $$status -ne 0
	@set +e; "build/shared_scratchpad_tb/Vtop" +CONFLICT_STORE_STORE >/dev/null 2>&1; status=$$?; test $$status -ne 0

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

compiler-e2e:
	cmake -S compiler -B "$(COMPILER_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON
	cmake --build "$(COMPILER_BUILD_DIR)" --target cgrac-compile-dfg
	mkdir -p "$(COMPILER_E2E_DIR)/compiler"
	"$(COMPILER_BUILD_DIR)/bin/cgrac-compile-dfg" "$(COMPILER_E2E_DFG)" \
		--target target/cgra_v3.json --trip-count 4 --max-ii 8 \
		--scratchpad-preload "$(COMPILER_E2E_PRELOAD)" \
		--artifact-dir "$(COMPILER_E2E_DIR)/compiler" -o "$(COMPILER_E2E_MANIFEST)"
	$(MAKE) --no-print-directory program BUILD_DIR="$(COMPILER_E2E_PROGRAM_DIR)" PROGRAM_MANIFEST="$(COMPILER_E2E_MANIFEST)"
	python3 tools/check_compiler_e2e_observations.py \
		--expectation "$(COMPILER_E2E_EXPECTATIONS)" \
		--golden "$(COMPILER_E2E_PROGRAM_DIR)/program/program_manifest/golden_trace.csv" \
		--rtl "$(COMPILER_E2E_PROGRAM_DIR)/program/program_manifest/rtl_trace.csv"
	python3 tools/write_compiler_e2e_report.py \
		--fixture fixed_addr_load_add_store --dfg "$(COMPILER_E2E_DFG)" \
		--target target/cgra_v3.json --manifest "$(COMPILER_E2E_MANIFEST)" \
		--compiler-artifacts "$(COMPILER_E2E_DIR)/compiler" \
		--program-dir "$(COMPILER_E2E_PROGRAM_DIR)/program/program_manifest" \
		--output "$(COMPILER_E2E_DIR)/e2e_result.json"

llvm-frontend-c-smoke:
	@set -eu; \
	command -v "$(LLVM_FRONTEND_CLANG)" >/dev/null || { echo "missing $(LLVM_FRONTEND_CLANG)"; exit 2; }; \
	command -v "$(LLVM_FRONTEND_OPT)" >/dev/null || { echo "missing $(LLVM_FRONTEND_OPT)"; exit 2; }; \
	cmake -S compiler -B "$(LLVM_FRONTEND_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON $(LLVM_CMAKE_ARG); \
	cmake --build "$(LLVM_FRONTEND_BUILD_DIR)" --target cgra-llvm-loop-lower --parallel; \
	case_dir="$(LLVM_FRONTEND_E2E_DIR)/c-smoke"; mkdir -p "$$case_dir/source" "$$case_dir/frontend"; \
	"$(LLVM_FRONTEND_CLANG)" -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -S -emit-llvm \
		compiler/tests/Frontend/LLVM/fixtures/kernel_scalar_add.c -o "$$case_dir/source/kernel.raw.ll"; \
	"$(LLVM_FRONTEND_OPT)" -S -passes='mem2reg,loop-simplify,lcssa,simplifycfg' \
		"$$case_dir/source/kernel.raw.ll" -o "$$case_dir/source/kernel.ll"; \
	"$(LLVM_FRONTEND_OPT)" -passes='mem2reg,loop-simplify,lcssa,simplifycfg' \
		"$$case_dir/source/kernel.raw.ll" -o "$$case_dir/source/kernel.bc"; \
	"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" "$$case_dir/source/kernel.ll" \
		--function kernel --artifact-dir "$$case_dir/frontend" -o "$$case_dir/frontend/generic_dfg.json"; \
	"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" "$$case_dir/source/kernel.bc" \
		--function kernel --loop-header do.body -o "$$case_dir/frontend/generic_dfg.bc.json"; \
	test -s "$$case_dir/frontend/generic_dfg.json"

llvm-frontend-e2e: llvm-frontend-c-smoke
	@set -eu; \
	case_dir="$(LLVM_FRONTEND_E2E_DIR)"; mkdir -p "$$case_dir"; \
	cmake --build "$(LLVM_FRONTEND_BUILD_DIR)" --target cgrac-compile-kernel --parallel; \
	compile_case() { \
		name="$$1"; source="$$2"; input="$$3"; expected="$$4"; output_name="$$5"; \
		root="$$case_dir/$$name"; mkdir -p "$$root/source" "$$root/frontend" "$$root/backend"; \
		"$(LLVM_FRONTEND_CLANG)" -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -S -emit-llvm "$$source" -o "$$root/source/kernel.raw.ll"; \
		"$(LLVM_FRONTEND_OPT)" -S -passes='mem2reg,loop-simplify,lcssa,simplifycfg' "$$root/source/kernel.raw.ll" -o "$$root/source/kernel.ll"; \
		printf '%s\n' '{"schema":"cgra.kernel_invocation.v1","trip_count":4,"scalar_inputs":{"x":'"$$input"'},"scratchpad_preload":[]}' > "$$root/invocation.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" "$$root/source/kernel.ll" --function kernel --artifact-dir "$$root/frontend" --invocation "$$root/invocation.json" -o "$$root/frontend/generic_dfg.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgrac-compile-kernel" "$$root/frontend/generic_dfg.json" --target target/cgra_v3.json --invocation "$$root/invocation.json" --artifact-dir "$$root/backend" -o "$$root/program_manifest.json"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$root" PROGRAM_MANIFEST="$$root/program_manifest.json"; \
		printf '%s\n' '{"schema":"cgra.kernel_abi.expectation.v1","outputs":{"'"$$output_name"'":{"final_value":'"$$expected"',"store_count":4}}}' > "$$root/expected.json"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$root/backend/01_kernel_signature.json" --layout "$$root/backend/04_kernel_abi_layout.json" --expectation "$$root/expected.json" --golden "$$root/program/program_manifest/golden_trace.csv" --rtl "$$root/program/program_manifest/rtl_trace.csv"; \
	}; \
	compile_case scalar_add_7 compiler/tests/Frontend/LLVM/fixtures/kernel_scalar_add.c 7 14 add; \
	compile_case scalar_add_9 compiler/tests/Frontend/LLVM/fixtures/kernel_scalar_add.c 9 18 add; \
	compile_case scalar_mul_7 compiler/tests/Frontend/LLVM/fixtures/kernel_scalar_mul.c 7 49 mul; \
	test "$$(sha256sum "$$case_dir/scalar_add_7/frontend/generic_dfg.json" | cut -d' ' -f1)" = "$$(sha256sum "$$case_dir/scalar_add_9/frontend/generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/scalar_add_7/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/scalar_add_9/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/scalar_add_7/frontend/generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/scalar_mul_7/frontend/generic_dfg.json" | cut -d' ' -f1)"; \
	python3 tools/write_compiler_e2e_report.py \
		--fixture llvm_frontend_scalar_add_7 --dfg "$$case_dir/scalar_add_7/frontend/generic_dfg.json" \
		--target target/cgra_v3.json --manifest "$$case_dir/scalar_add_7/program_manifest.json" \
		--compiler-artifacts "$$case_dir/scalar_add_7/backend/backend" \
		--program-dir "$$case_dir/scalar_add_7/program/program_manifest" \
		--output "$$case_dir/e2e_result.json"

llvm-recurrence-e2e:
	@set -eu; \
	case_dir="$(LLVM_FRONTEND_E2E_DIR)/recurrence"; mkdir -p "$$case_dir"; \
	cmake -S compiler -B "$(LLVM_FRONTEND_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON $(LLVM_CMAKE_ARG); \
	cmake --build "$(LLVM_FRONTEND_BUILD_DIR)" --target cgra-llvm-loop-lower cgrac-compile-kernel --parallel; \
	mkdir -p "$$case_dir/c-smoke/source" "$$case_dir/c-smoke/frontend"; \
	"$(LLVM_FRONTEND_CLANG)" -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -S -emit-llvm compiler/tests/Frontend/LLVM/fixtures/kernel_recurrence.c -o "$$case_dir/c-smoke/source/kernel.raw.ll"; \
	"$(LLVM_FRONTEND_OPT)" -S -passes='mem2reg,loop-simplify,lcssa,simplifycfg' "$$case_dir/c-smoke/source/kernel.raw.ll" -o "$$case_dir/c-smoke/source/kernel.ll"; \
	"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" "$$case_dir/c-smoke/source/kernel.ll" --function kernel --artifact-dir "$$case_dir/c-smoke/frontend" -o "$$case_dir/c-smoke/frontend/generic_dfg.json"; \
	compile_case() { \
		name="$$1"; seed="$$2"; trips="$$3"; expected="$$4"; \
		root="$$case_dir/$$name"; mkdir -p "$$root/frontend" "$$root/backend"; \
		printf '%s\n' '{"schema":"cgra.kernel_invocation.v1","trip_count":'"$$trips"',"scalar_inputs":{"seed":'"$$seed"'},"scratchpad_preload":[]}' > "$$root/invocation.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" compiler/tests/Frontend/LLVM/fixtures/recurrence_seed.ll --function kernel --artifact-dir "$$root/frontend" --invocation "$$root/invocation.json" -o "$$root/frontend/generic_dfg.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgrac-compile-kernel" "$$root/frontend/generic_dfg.json" --target target/cgra_v3.json --invocation "$$root/invocation.json" --max-ii 8 --artifact-dir "$$root/backend" -o "$$root/program_manifest.json"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$root" PROGRAM_MANIFEST="$$root/program_manifest.json"; \
		printf '%s\n' '{"schema":"cgra.kernel_abi.expectation.v1","outputs":{"next":{"final_value":'"$$expected"',"store_count":'"$$trips"'}}}' > "$$root/expected.json"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$root/backend/01_kernel_signature.json" --layout "$$root/backend/04_kernel_abi_layout.json" --expectation "$$root/expected.json" --golden "$$root/program/program_manifest/golden_trace.csv" --rtl "$$root/program/program_manifest/rtl_trace.csv"; \
	}; \
	compile_case seed5_trip1 5 1 6; \
	compile_case seed5_trip4 5 4 9; \
	compile_case seed5_trip7 5 7 12; \
	compile_case seed20_trip4 20 4 24; \
	test "$$(sha256sum "$$case_dir/seed5_trip4/frontend/generic_dfg.json" | cut -d' ' -f1)" = "$$(sha256sum "$$case_dir/seed20_trip4/frontend/generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/seed5_trip4/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/seed20_trip4/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"; \
	compile_induction_case() { \
		name="$$1"; input="$$2"; expected="$$3"; root="$$case_dir/$$name"; \
		mkdir -p "$$root/frontend" "$$root/backend"; \
		printf '%s\n' '{"schema":"cgra.kernel_invocation.v1","trip_count":4,"scalar_inputs":{"x":'"$$input"'},"scratchpad_preload":[]}' > "$$root/invocation.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" compiler/tests/Frontend/LLVM/fixtures/induction_data_use_v0.ll --function kernel --artifact-dir "$$root/frontend" --invocation "$$root/invocation.json" -o "$$root/frontend/generic_dfg.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgrac-compile-kernel" "$$root/frontend/generic_dfg.json" --target target/cgra_v3.json --invocation "$$root/invocation.json" --max-ii 8 --max-node-candidates 10000 --max-backtracks 10000 --max-route-calls 20000 --max-route-states 5000 --artifact-dir "$$root/backend" -o "$$root/program_manifest.json"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$root" PROGRAM_MANIFEST="$$root/program_manifest.json"; \
		printf '%s\n' '{"schema":"cgra.kernel_abi.expectation.v1","outputs":{"y":{"final_value":'"$$expected"',"store_count":4}}}' > "$$root/expected.json"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$root/backend/01_kernel_signature.json" --layout "$$root/backend/04_kernel_abi_layout.json" --expectation "$$root/expected.json" --golden "$$root/program/program_manifest/golden_trace.csv" --rtl "$$root/program/program_manifest/rtl_trace.csv"; \
	}; \
	compile_induction_case induction_x7 7 10; \
	compile_induction_case induction_x20 20 23; \
	test "$$(sha256sum "$$case_dir/induction_x7/frontend/generic_dfg.json" | cut -d' ' -f1)" = "$$(sha256sum "$$case_dir/induction_x20/frontend/generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/induction_x7/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/induction_x20/backend/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/induction_x7/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/induction_x20/program_manifest.json" | cut -d' ' -f1)"; \
	true

llvm-predication-e2e:
	@set -eu; \
	case_dir="$(LLVM_FRONTEND_E2E_DIR)/predication"; mkdir -p "$$case_dir"; \
	cmake -S compiler -B "$(LLVM_FRONTEND_BUILD_DIR)" -DCGRA_BUILD_TESTS=ON -DCGRA_WARNINGS_AS_ERRORS=ON $(LLVM_CMAKE_ARG); \
	cmake --build "$(LLVM_FRONTEND_BUILD_DIR)" --target cgra-llvm-frontend-tests cgra-llvm-loop-lower cgrac-compile-kernel --parallel; \
	"$(LLVM_FRONTEND_BUILD_DIR)/cgra-llvm-frontend-tests"; \
	compile_case() { \
		name="$$1"; input="$$2"; expected="$$3"; root="$$case_dir/$$name"; \
		mkdir -p "$$root/frontend" "$$root/backend"; \
		printf '%s\n' '{"schema":"cgra.kernel_invocation.v1","trip_count":1,"scalar_inputs":{"x":'"$$input"'},"scratchpad_preload":[]}' > "$$root/invocation.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" compiler/tests/Frontend/LLVM/fixtures/predication_one_input.ll --function one_input --artifact-dir "$$root/frontend" --invocation "$$root/invocation.json" -o "$$root/frontend/generic_dfg.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgrac-compile-kernel" "$$root/frontend/generic_dfg.json" --target target/cgra_v3.json --invocation "$$root/invocation.json" --artifact-dir "$$root/backend" -o "$$root/program_manifest.json"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$root" PROGRAM_MANIFEST="$$root/program_manifest.json"; \
		printf '%s\n' '{"schema":"cgra.kernel_abi.expectation.v1","outputs":{"v":{"final_value":'"$$expected"',"store_count":1}}}' > "$$root/expected.json"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$root/backend/01_kernel_signature.json" --layout "$$root/backend/04_kernel_abi_layout.json" --expectation "$$root/expected.json" --golden "$$root/program/program_manifest/golden_trace.csv" --rtl "$$root/program/program_manifest/rtl_trace.csv"; \
		test -s "$$root/frontend/02_if_conversion.json"; \
	}; \
	compile_case value_branch_zero 0 0; \
	compile_case value_branch_nonzero 7 14; \
	compile_store_case() { \
		name="$$1"; invocation="$$2"; expectation="$$3"; root="$$case_dir/$$name"; \
		mkdir -p "$$root/frontend" "$$root/backend"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgra-llvm-loop-lower" compiler/tests/Frontend/LLVM/fixtures/predicated_store_v0.ll --function predicated_store_v0 --artifact-dir "$$root/frontend" --invocation "$$invocation" -o "$$root/frontend/generic_dfg.json"; \
		"$(LLVM_FRONTEND_BUILD_DIR)/bin/cgrac-compile-kernel" "$$root/frontend/generic_dfg.json" --target target/cgra_v3.json --invocation "$$invocation" --artifact-dir "$$root/backend" --max-ii 8 --max-node-candidates 10000 --max-backtracks 10000 --max-route-calls 20000 --max-route-states 5000 -o "$$root/program_manifest.json"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$root" PROGRAM_MANIFEST="$$root/program_manifest.json"; \
		python3 tools/check_predicated_store_e2e.py --layout "$$root/backend/04_kernel_abi_layout.json" --expectation "$$expectation" --golden "$$root/program/program_manifest/golden_trace.csv" --rtl "$$root/program/program_manifest/rtl_trace.csv"; \
	}; \
	compile_store_case store_limit2 compiler/tests/Frontend/LLVM/fixtures/predicated_store_limit2.json compiler/tests/Frontend/LLVM/fixtures/predicated_store_limit2_expected.json; \
	compile_store_case store_limit0 compiler/tests/Frontend/LLVM/fixtures/predicated_store_limit0.json compiler/tests/Frontend/LLVM/fixtures/predicated_store_limit0_expected.json; \
	test "$$(sha256sum "$$case_dir/store_limit2/frontend/generic_dfg.json" | cut -d' ' -f1)" = "$$(sha256sum "$$case_dir/store_limit0/frontend/generic_dfg.json" | cut -d' ' -f1)"; \
	test "$$(sha256sum "$$case_dir/store_limit2/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$$case_dir/store_limit0/program_manifest.json" | cut -d' ' -f1)"

kernel-abi-scalar-e2e:
	cmake -S compiler -B "$(COMPILER_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON
	cmake --build "$(COMPILER_BUILD_DIR)" --target cgrac-compile-kernel
	@set -e; for suffix in 7 9; do \
		case "$$suffix" in \
			7) invocation=compiler/tests/ABI/fixtures/abi_scalar_input_7.json; expectation=compiler/tests/ABI/fixtures/abi_scalar_input_7_expected.json ;; \
			9) invocation=compiler/tests/ABI/fixtures/abi_scalar_input_9.json; expectation=compiler/tests/ABI/fixtures/abi_scalar_input_9_expected.json ;; \
		esac; \
		case_dir="$(COMPILER_KERNEL_SCALAR_DIR)/$$suffix"; manifest="$$case_dir/program_manifest.json"; \
		mkdir -p "$$case_dir/compiler"; \
		"$(COMPILER_BUILD_DIR)/bin/cgrac-compile-kernel" compiler/tests/ABI/fixtures/abi_scalar_input.dfg.json --target target/cgra_v3.json --invocation "$$invocation" --artifact-dir "$$case_dir/compiler" -o "$$manifest"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$case_dir" PROGRAM_MANIFEST="$$manifest"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$case_dir/compiler/01_kernel_signature.json" --layout "$$case_dir/compiler/04_kernel_abi_layout.json" --expectation "$$expectation" --golden "$$case_dir/program/program_manifest/golden_trace.csv" --rtl "$$case_dir/program/program_manifest/rtl_trace.csv"; \
	done
	test "$$(sha256sum "$(COMPILER_KERNEL_SCALAR_DIR)/7/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_SCALAR_DIR)/9/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"
	test "$$(sha256sum "$(COMPILER_KERNEL_SCALAR_DIR)/7/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_SCALAR_DIR)/9/program_manifest.json" | cut -d' ' -f1)"

kernel-abi-base-load-e2e:
	cmake -S compiler -B "$(COMPILER_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON
	cmake --build "$(COMPILER_BUILD_DIR)" --target cgrac-compile-kernel
	@set -e; for suffix in a b; do \
		case "$$suffix" in \
			a) invocation=compiler/tests/ABI/fixtures/abi_base_load_a.json; expectation=compiler/tests/ABI/fixtures/abi_base_load_a_expected.json ;; \
			b) invocation=compiler/tests/ABI/fixtures/abi_base_load_b.json; expectation=compiler/tests/ABI/fixtures/abi_base_load_b_expected.json ;; \
		esac; \
		case_dir="$(COMPILER_KERNEL_BASE_DIR)/$$suffix"; manifest="$$case_dir/program_manifest.json"; \
		mkdir -p "$$case_dir/compiler"; \
		"$(COMPILER_BUILD_DIR)/bin/cgrac-compile-kernel" compiler/tests/ABI/fixtures/abi_base_load.dfg.json --target target/cgra_v3.json --invocation "$$invocation" --artifact-dir "$$case_dir/compiler" -o "$$manifest"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$case_dir" PROGRAM_MANIFEST="$$manifest"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$case_dir/compiler/01_kernel_signature.json" --layout "$$case_dir/compiler/04_kernel_abi_layout.json" --expectation "$$expectation" --golden "$$case_dir/program/program_manifest/golden_trace.csv" --rtl "$$case_dir/program/program_manifest/rtl_trace.csv"; \
	done
	test "$$(sha256sum "$(COMPILER_KERNEL_BASE_DIR)/a/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_BASE_DIR)/b/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"
	test "$$(sha256sum "$(COMPILER_KERNEL_BASE_DIR)/a/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_BASE_DIR)/b/program_manifest.json" | cut -d' ' -f1)"

kernel-abi-recurrence-e2e:
	cmake -S compiler -B "$(COMPILER_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON
	cmake --build "$(COMPILER_BUILD_DIR)" --target cgrac-compile-kernel
	@set -e; for suffix in 5 20; do \
		case "$$suffix" in \
			5) invocation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5.json; expectation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5_expected.json ;; \
			20) invocation=compiler/tests/ABI/fixtures/abi_recurrence_seed_20.json; expectation=compiler/tests/ABI/fixtures/abi_recurrence_seed_20_expected.json ;; \
		esac; \
		case_dir="$(COMPILER_KERNEL_RECURRENCE_DIR)/$$suffix"; manifest="$$case_dir/program_manifest.json"; \
		mkdir -p "$$case_dir/compiler"; \
		"$(COMPILER_BUILD_DIR)/bin/cgrac-compile-kernel" compiler/tests/ABI/fixtures/abi_recurrence_seed_two_node.dfg.json --target target/cgra_v3.json --invocation "$$invocation" --artifact-dir "$$case_dir/compiler" -o "$$manifest"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$case_dir" PROGRAM_MANIFEST="$$manifest"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$case_dir/compiler/01_kernel_signature.json" --layout "$$case_dir/compiler/04_kernel_abi_layout.json" --expectation "$$expectation" --golden "$$case_dir/program/program_manifest/golden_trace.csv" --rtl "$$case_dir/program/program_manifest/rtl_trace.csv"; \
	done
	test "$$(sha256sum "$(COMPILER_KERNEL_RECURRENCE_DIR)/5/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_RECURRENCE_DIR)/20/compiler/03_abi_bound.generic_dfg.json" | cut -d' ' -f1)"
	test "$$(sha256sum "$(COMPILER_KERNEL_RECURRENCE_DIR)/5/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_RECURRENCE_DIR)/20/program_manifest.json" | cut -d' ' -f1)"

kernel-abi-tripcount-e2e:
	cmake -S compiler -B "$(COMPILER_BUILD_DIR)" -DCGRA_BUILD_TESTS=OFF -DCGRA_WARNINGS_AS_ERRORS=ON
	cmake --build "$(COMPILER_BUILD_DIR)" --target cgrac-compile-kernel
	@set -e; for suffix in trip1 trip7; do \
		case "$$suffix" in \
			trip1) invocation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5_trip1.json; expectation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5_trip1_expected.json ;; \
			trip7) invocation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5_trip7.json; expectation=compiler/tests/ABI/fixtures/abi_recurrence_seed_5_trip7_expected.json ;; \
		esac; \
		case_dir="$(COMPILER_KERNEL_TRIPCOUNT_DIR)/$$suffix"; manifest="$$case_dir/program_manifest.json"; \
		mkdir -p "$$case_dir/compiler"; \
		"$(COMPILER_BUILD_DIR)/bin/cgrac-compile-kernel" compiler/tests/ABI/fixtures/abi_recurrence_seed_two_node.dfg.json --target target/cgra_v3.json --invocation "$$invocation" --artifact-dir "$$case_dir/compiler" -o "$$manifest"; \
		$(MAKE) --no-print-directory program BUILD_DIR="$$case_dir" PROGRAM_MANIFEST="$$manifest"; \
		python3 tools/check_kernel_abi_e2e.py --signature "$$case_dir/compiler/01_kernel_signature.json" --layout "$$case_dir/compiler/04_kernel_abi_layout.json" --expectation "$$expectation" --golden "$$case_dir/program/program_manifest/golden_trace.csv" --rtl "$$case_dir/program/program_manifest/rtl_trace.csv"; \
	done
	test "$$(sha256sum "$(COMPILER_KERNEL_TRIPCOUNT_DIR)/trip1/program_manifest.json" | cut -d' ' -f1)" != "$$(sha256sum "$(COMPILER_KERNEL_TRIPCOUNT_DIR)/trip7/program_manifest.json" | cut -d' ' -f1)"

kernel-abi-e2e: kernel-abi-scalar-e2e kernel-abi-base-load-e2e kernel-abi-recurrence-e2e kernel-abi-tripcount-e2e

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
	for arg in +INVALID_LOOP_II +INVALID_LOOP_SPAN +PARTIAL_LOOP_DESC +LOOP_DESC_DURING_RUN +LOOP_DESC_DURING_DONE; do \
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
