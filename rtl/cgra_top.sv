// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

function automatic logic [1023:0] cgra_default_lsu_mask(input int rows, input int cols);
  logic [1023:0] mask;
  begin
    mask = '0;
    for (int r = 0; r < rows; r++) begin
      mask[r * cols] = 1'b1;
    end
    cgra_default_lsu_mask = mask;
  end
endfunction

module cgra_top #(
  parameter int ROWS = cgra_pkg::ARRAY_ROWS,
  parameter int COLS = cgra_pkg::ARRAY_COLS,
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int TILES = ROWS * COLS,
  parameter int CONTROL_WIDTH = cgra_pkg::CONTROL_WORD_PHYSICAL_WIDTH,
  parameter logic [TILES-1:0] HAS_LSU_MASK = TILES'(cgra_default_lsu_mask(ROWS, COLS))
) (
  input  logic                         clk,
  input  logic                         rst_n,

  input  logic                         cfg_valid,
  output logic                         cfg_ready,
  input  logic                         cfg_we,
  input  logic [1:0]                   cfg_mem_type,
  input  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] cfg_tile_row,
  input  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] cfg_tile_col,
  input  logic [cgra_pkg::SCRATCH_ADDR_WIDTH-1:0] cfg_addr,
  input  logic [1:0]                   cfg_word_idx,
  input  logic [31:0]                  cfg_wdata,

  input  logic                         start,
  input  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] run_cycles,
  output logic                         busy,
  output logic                         done,
  output logic [cgra_pkg::CTRL_PC_WIDTH-1:0] kernel_pc,

  output logic [TILES-1:0]             north_data_we,
  output logic [TILES*DATA_WIDTH-1:0]  north_data_out,
  output logic [TILES-1:0]             south_data_we,
  output logic [TILES*DATA_WIDTH-1:0]  south_data_out,
  output logic [TILES-1:0]             east_data_we,
  output logic [TILES*DATA_WIDTH-1:0]  east_data_out,
  output logic [TILES-1:0]             west_data_we,
  output logic [TILES*DATA_WIDTH-1:0]  west_data_out
);
  localparam logic [1:0] CFG_MEM_CONTROL = 2'd0;
  localparam logic [1:0] CFG_MEM_CONST = 2'd1;
  localparam logic [1:0] CFG_MEM_SCRATCH = 2'd2;
  localparam logic [1:0] CFG_MEM_LOOP_DESC = 2'd3;
  localparam int RUN_CONTROL_SLICE_WIDTH = 16;
  localparam int RUN_CONTROL_REPLICAS = CONTROL_WIDTH / RUN_CONTROL_SLICE_WIDTH;

  localparam logic [1:0] LOOP_PHASE_FLAT = 2'd0;
  localparam logic [1:0] LOOP_PHASE_PROLOGUE = 2'd1;
  localparam logic [1:0] LOOP_PHASE_KERNEL = 2'd2;
  localparam logic [1:0] LOOP_PHASE_EPILOGUE = 2'd3;

  typedef enum logic [1:0] {
    STATE_CONFIG = 2'd0,
    STATE_RUN    = 2'd1,
    STATE_DONE   = 2'd2
  } state_e;

  state_e state;
  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] cycles_left;
  logic [31:0] loop_prologue_shadow;
  logic [31:0] loop_ii_shadow;
  logic [31:0] loop_trip_count_shadow;
  logic [31:0] loop_epilogue_shadow;
  logic [31:0] loop_prologue_cycles;
  logic [31:0] loop_ii_cycles;
  logic [31:0] loop_trip_count;
  logic [31:0] loop_epilogue_cycles;
  logic loop_committed;
  logic [1:0] loop_phase;
  logic [31:0] loop_phase_count;
  logic [31:0] loop_iterations_left;
  logic [31:0] loop_iteration;
  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] loop_kernel_slot;
  logic loop_last_cycle;
  logic [TILES*CONTROL_WIDTH-1:0] mesh_control_words;
  logic [TILES*CONTROL_WIDTH-1:0] control_mem_read_data;
  logic [TILES-1:0] control_cfg_we;
  logic [TILES-1:0] const_cfg_we;
  logic [TILES*cgra_pkg::CONST_ADDR_WIDTH-1:0] const_cfg_addr;
  logic [TILES*DATA_WIDTH-1:0] const_cfg_wdata;
  logic [TILES-1:0] scratch_cfg_we;
  logic [TILES*cgra_pkg::SCRATCH_ADDR_WIDTH-1:0] scratch_cfg_addr;
  logic [TILES*DATA_WIDTH-1:0] scratch_cfg_wdata;
  logic [TILES-1:0] north_pred_we_unused;
  logic [TILES*cgra_pkg::PRED_WIDTH-1:0] north_pred_out_unused;
  logic [TILES-1:0] south_pred_we_unused;
  logic [TILES*cgra_pkg::PRED_WIDTH-1:0] south_pred_out_unused;
  logic [TILES-1:0] east_pred_we_unused;
  logic [TILES*cgra_pkg::PRED_WIDTH-1:0] east_pred_out_unused;
  logic [TILES-1:0] west_pred_we_unused;
  logic [TILES*cgra_pkg::PRED_WIDTH-1:0] west_pred_out_unused;
  logic unused_pred_outputs;
  logic unused_cfg_tile_idx_upper;

  logic cfg_accept;
  logic loop_desc_cfg_accept;
  logic cfg_in_bounds;
  logic next_run_active;
  logic [31:0] cfg_tile_idx_full;
  logic [$clog2(TILES)-1:0] cfg_tile_idx;
  logic [cgra_pkg::CTRL_PC_WIDTH-1:0] cfg_ctrl_addr;

  function automatic logic loop_descriptor_valid(
    input logic [31:0] prologue_cycles,
    input logic [31:0] ii_cycles,
    input logic [31:0] trip_count,
    input logic [31:0] epilogue_cycles
  );
    logic [63:0] span;
    begin
      span = {32'd0, prologue_cycles}
             + {32'd0, ii_cycles}
             + {32'd0, epilogue_cycles};
      loop_descriptor_valid = (ii_cycles != 0)
                              && (trip_count != 0)
                              && (span <= 64'(cgra_pkg::CTRL_MEM_DEPTH));
    end
  endfunction

  assign cfg_tile_idx_full = (32'(cfg_tile_row) * 32'(COLS)) + 32'(cfg_tile_col);
  assign cfg_tile_idx = cfg_tile_idx_full[$clog2(TILES)-1:0];
  assign cfg_in_bounds = (32'(cfg_tile_row) < 32'(ROWS)) && (32'(cfg_tile_col) < 32'(COLS));
  assign cfg_ctrl_addr = cfg_addr[cgra_pkg::CTRL_PC_WIDTH-1:0];
  assign cfg_ready = (state != STATE_RUN);
  assign cfg_accept = cfg_valid && cfg_ready && cfg_we && cfg_in_bounds;
  assign loop_desc_cfg_accept = (state == STATE_CONFIG)
                                && cfg_accept
                                && (cfg_mem_type == CFG_MEM_LOOP_DESC)
                                && (cfg_tile_row == '0)
                                && (cfg_tile_col == '0)
                                && (cfg_word_idx == 2'd0)
                                && (cfg_addr <= 10'd4);
  assign busy = (state == STATE_RUN);
  assign done = (state == STATE_DONE);
  always_comb begin
    loop_last_cycle = 1'b0;
    if (loop_committed) begin
      unique case (loop_phase)
        LOOP_PHASE_KERNEL: begin
          loop_last_cycle = ((32'(loop_kernel_slot) + 32'd1) >= loop_ii_cycles)
                            && (loop_iterations_left <= 32'd1)
                            && (loop_epilogue_cycles == 0);
        end
        LOOP_PHASE_EPILOGUE: begin
          loop_last_cycle = (loop_phase_count <= 32'd1);
        end
        default: begin
        end
      endcase
    end
  end
  assign unused_cfg_tile_idx_upper = ^cfg_tile_idx_full[31:$clog2(TILES)];
  assign unused_pred_outputs = (|north_pred_we_unused)
                               ^ (^north_pred_out_unused)
                               ^ (|south_pred_we_unused)
                               ^ (^south_pred_out_unused)
                               ^ (|east_pred_we_unused)
                               ^ (^east_pred_out_unused)
                               ^ (|west_pred_we_unused)
                               ^ (^west_pred_out_unused);

  always_comb begin
    next_run_active = 1'b0;
    unique case (state)
      STATE_CONFIG: begin
        if (start) begin
          next_run_active = 1'b1;
        end
      end
      STATE_RUN: begin
        next_run_active = 1'b1;
        if (loop_committed ? loop_last_cycle : (cycles_left <= 1)) begin
          next_run_active = 1'b0;
        end
      end
      default: begin
      end
    endcase
  end

  always_comb begin
    control_cfg_we = '0;
    const_cfg_we = '0;
    const_cfg_addr = '0;
    const_cfg_wdata = '0;
    scratch_cfg_we = '0;
    scratch_cfg_addr = '0;
    scratch_cfg_wdata = '0;
    if ((state == STATE_CONFIG) && cfg_accept && (cfg_mem_type == CFG_MEM_CONTROL)) begin
      control_cfg_we[cfg_tile_idx] = 1'b1;
    end

    if (cfg_accept && (cfg_mem_type == CFG_MEM_CONST)) begin
      const_cfg_we[cfg_tile_idx] = 1'b1;
      const_cfg_addr[cfg_tile_idx*cgra_pkg::CONST_ADDR_WIDTH +: cgra_pkg::CONST_ADDR_WIDTH] = cfg_addr[cgra_pkg::CONST_ADDR_WIDTH-1:0];
      const_cfg_wdata[cfg_tile_idx*DATA_WIDTH +: DATA_WIDTH] = cfg_wdata;
    end

    if (cfg_accept && (cfg_mem_type == CFG_MEM_SCRATCH)) begin
      scratch_cfg_we[cfg_tile_idx] = 1'b1;
      scratch_cfg_addr[cfg_tile_idx*cgra_pkg::SCRATCH_ADDR_WIDTH +: cgra_pkg::SCRATCH_ADDR_WIDTH] = cfg_addr[cgra_pkg::SCRATCH_ADDR_WIDTH-1:0];
      scratch_cfg_wdata[cfg_tile_idx*DATA_WIDTH +: DATA_WIDTH] = cfg_wdata;
    end

    if (unused_pred_outputs || unused_cfg_tile_idx_upper) begin
    end
  end

  genvar control_mem_idx;
  generate
    for (control_mem_idx = 0; control_mem_idx < TILES; control_mem_idx = control_mem_idx + 1) begin : control_mem_bank_gen
      for (genvar run_replica_idx = 0;
           run_replica_idx < RUN_CONTROL_REPLICAS;
           run_replica_idx = run_replica_idx + 1) begin : run_gate_gen
        (* keep = "true" *) logic run_active;
        logic [RUN_CONTROL_SLICE_WIDTH-1:0] gated_control_slice;

        (* keep = "true" *) always_ff @(posedge clk) begin
          if (!rst_n) begin
            run_active <= 1'b0;
          end else begin
            run_active <= next_run_active;
          end
        end

        always_comb begin
          if (run_active) begin
            gated_control_slice = control_mem_read_data[
              control_mem_idx*CONTROL_WIDTH + run_replica_idx*RUN_CONTROL_SLICE_WIDTH
              +: RUN_CONTROL_SLICE_WIDTH
            ];
          end else begin
            gated_control_slice = '0;
          end
        end

`ifndef SYNTHESIS
        always_ff @(posedge clk) begin
          if (rst_n && (run_active !== (state == STATE_RUN))) begin
            $fatal(1, "RUN replica diverged from architectural state: tile=%0d replica=%0d",
                   control_mem_idx, run_replica_idx);
          end
        end
`endif

        assign mesh_control_words[
          control_mem_idx*CONTROL_WIDTH + run_replica_idx*RUN_CONTROL_SLICE_WIDTH
          +: RUN_CONTROL_SLICE_WIDTH
        ] = gated_control_slice;
      end

      control_mem_bank #(
        .WIDTH(CONTROL_WIDTH),
        .DEPTH(cgra_pkg::CTRL_MEM_DEPTH),
        .ADDR_WIDTH(cgra_pkg::CTRL_PC_WIDTH)
      ) control_mem_i (
        .clk(clk),
        .cfg_write_en(control_cfg_we[control_mem_idx]),
        .cfg_write_addr(cfg_ctrl_addr),
        .cfg_write_word_idx(cfg_word_idx),
        .cfg_write_data(cfg_wdata),
        .read_addr(kernel_pc),
        .read_data(control_mem_read_data[control_mem_idx*CONTROL_WIDTH +: CONTROL_WIDTH])
      );
    end
  endgenerate

  mesh #(
    .ROWS(ROWS),
    .COLS(COLS),
    .DATA_WIDTH(DATA_WIDTH),
    .HAS_LSU_MASK(HAS_LSU_MASK)
  ) mesh_i (
    .clk(clk),
    .rst_n(rst_n),
    .control_words(mesh_control_words),
    .const_cfg_we(const_cfg_we),
    .const_cfg_addr(const_cfg_addr),
    .const_cfg_wdata(const_cfg_wdata),
    .scratch_cfg_we(scratch_cfg_we),
    .scratch_cfg_addr(scratch_cfg_addr),
    .scratch_cfg_wdata(scratch_cfg_wdata),
    .north_data_we(north_data_we),
    .north_data_out(north_data_out),
    .south_data_we(south_data_we),
    .south_data_out(south_data_out),
    .east_data_we(east_data_we),
    .east_data_out(east_data_out),
    .west_data_we(west_data_we),
    .west_data_out(west_data_out),
    .north_pred_we(north_pred_we_unused),
    .north_pred_out(north_pred_out_unused),
    .south_pred_we(south_pred_we_unused),
    .south_pred_out(south_pred_out_unused),
    .east_pred_we(east_pred_we_unused),
    .east_pred_out(east_pred_out_unused),
    .west_pred_we(west_pred_we_unused),
    .west_pred_out(west_pred_out_unused)
  );

`ifndef SYNTHESIS
  integer trace_fd;
  logic trace_enabled;
  logic trace_loop_enabled;
  int trace_cycle;
  string trace_path;

  initial begin
    trace_fd = 0;
    trace_enabled = ($test$plusargs("CGRA_TRACE") != 0);
    trace_loop_enabled = ($test$plusargs("CGRA_LOOP_TRACE") != 0);
    trace_cycle = 0;
    trace_path = "sim/build/trace_tb/trace.csv";
    if (trace_enabled) begin
      if (!$value$plusargs("CGRA_TRACE_FILE=%s", trace_path)) begin
        trace_path = "sim/build/trace_tb/trace.csv";
      end
      trace_fd = $fopen(trace_path, "w");
      if (trace_fd == 0) begin
        $fatal(1, "Unable to open CGRA trace file: %s", trace_path);
      end
      $fwrite(trace_fd, "cycle,kernel_pc,tile_row,tile_col,op,");
      $fwrite(trace_fd, "src_a_valid,src_a_value,src_b_valid,src_b_value,");
      $fwrite(trace_fd, "src_p0_valid,src_p0_value,src_p1_valid,src_p1_value,");
      $fwrite(trace_fd, "fu_data_valid,fu_data_result,fu_pred_valid,fu_pred_result,");
      $fwrite(trace_fd, "data_w0_we,data_w0_addr,data_w0_data,");
      $fwrite(trace_fd, "data_w1_we,data_w1_addr,data_w1_data,");
      $fwrite(trace_fd, "pred_w0_we,pred_w0_addr,pred_w0_data,");
      $fwrite(trace_fd, "pred_w1_we,pred_w1_addr,pred_w1_data,");
      $fwrite(trace_fd, "data_out_n_valid,data_out_n_value,");
      $fwrite(trace_fd, "data_out_s_valid,data_out_s_value,");
      $fwrite(trace_fd, "data_out_e_valid,data_out_e_value,");
      $fwrite(trace_fd, "data_out_w_valid,data_out_w_value,");
      $fwrite(trace_fd, "pred_out_n_valid,pred_out_n_value,");
      $fwrite(trace_fd, "pred_out_s_valid,pred_out_s_value,");
      $fwrite(trace_fd, "pred_out_e_valid,pred_out_e_value,");
      $fwrite(trace_fd, "pred_out_w_valid,pred_out_w_value,");
      $fwrite(trace_fd, "lsu_op,lsu_addr,lsu_store_data,lsu_store_commit,");
      $fwrite(trace_fd, "lsu_load_resp_valid,lsu_load_resp_data");
      if (trace_loop_enabled) begin
        $fwrite(trace_fd, ",loop_phase,loop_iteration,kernel_slot\n");
      end else begin
        $fwrite(trace_fd, "\n");
      end
    end
  end

  final begin
    if (trace_fd != 0) begin
      $fclose(trace_fd);
    end
  end

  always_ff @(posedge clk) begin
    if (!rst_n || (state != STATE_RUN)) begin
      trace_cycle <= 0;
    end else if (trace_enabled) begin
      trace_cycle <= trace_cycle + 1;
    end
  end

  genvar trace_row;
  genvar trace_col;
  generate
    for (trace_row = 0; trace_row < ROWS; trace_row = trace_row + 1) begin : trace_row_gen
      for (trace_col = 0; trace_col < COLS; trace_col = trace_col + 1) begin : trace_col_gen
        localparam int TRACE_ROW = trace_row;
        localparam int TRACE_COL = trace_col;
        always_ff @(posedge clk) begin
          if (rst_n && trace_enabled && (state == STATE_RUN)) begin
            $fwrite(trace_fd,
                    "%0d,%0d,%0d,%0d,%0d,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0d,%0h,%0d,%0d,%0h,%0d,%0d,%0h,%0d,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h,%0d,%0h",
                    trace_cycle,
                    kernel_pc,
                    TRACE_ROW,
                    TRACE_COL,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.exec_ctrl.op,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_data_a
                      && mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.data_src_a_valid_unused,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_data_a
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.data_src_a : DATA_WIDTH'(0),
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_data_b
                      && mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.data_src_b_valid_unused,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_data_b
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.data_src_b : DATA_WIDTH'(0),
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_pred_p0
                      && mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.pred_src_p0_valid_unused,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_pred_p0
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.pred_src_p0 : 1'b0,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_pred_p1
                      && mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.pred_src_p1_valid_unused,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.op_uses_pred_p1
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.pred_src_p1 : 1'b0,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_data_result_valid,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_data_result,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_pred_result_valid,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_pred_result,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.data_rf_ctrl.data_w0_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.data_rf_ctrl.data_w0_addr,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_data_result,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.data_rf_ctrl.data_w1_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.data_rf_ctrl.data_w1_addr,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.data_rf_ctrl.data_w1_we
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.data_w1_data : DATA_WIDTH'(0),
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.pred_rf_ctrl.pred_w0_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.pred_rf_ctrl.pred_w0_addr,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.fu_pred_result,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.pred_rf_ctrl.pred_w1_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.pred_rf_ctrl.pred_w1_addr,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.pred_rf_ctrl.pred_w1_we
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.pred_w1_data : 1'b0,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_north_data_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_north_data_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_south_data_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_south_data_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_east_data_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_east_data_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_west_data_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_west_data_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_north_pred_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_north_pred_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_south_pred_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_south_pred_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_east_pred_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_east_pred_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_west_pred_we,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.route_west_pred_out,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.lsu_ctrl.lsu_op,
                    ((mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_LOAD)
                     || (mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE))
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.lsu_i.addr_data : DATA_WIDTH'(0),
                    (mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE)
                      ? mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.lsu_i.store_data : DATA_WIDTH'(0),
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.lsu_i.store_commit,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.tile_lsu_load_valid,
                    mesh_i.row_gen[TRACE_ROW].col_gen[TRACE_COL].tile_i.tile_lsu_load_data);
            if (trace_loop_enabled) begin
              $fwrite(trace_fd, ",%0d,%0d,%0d\n",
                      loop_phase, loop_iteration, loop_kernel_slot);
            end else begin
              $fwrite(trace_fd, "\n");
            end
          end
        end
      end
    end
  endgenerate

  always_comb begin
    if (cfg_valid && cfg_ready && cfg_we && !cfg_in_bounds) begin
      $fatal(1, "Configuration target tile out of range: row=%0d col=%0d", cfg_tile_row, cfg_tile_col);
    end
    if (cfg_valid && cfg_ready && cfg_we && (cfg_mem_type == CFG_MEM_CONTROL)
        && (32'(cfg_word_idx) >= 32'(cgra_pkg::CONTROL_WORD_CHUNKS))) begin
      $fatal(1, "Configuration control word index out of range: idx=%0d", cfg_word_idx);
    end
    if (cfg_valid && cfg_we && (cfg_mem_type == CFG_MEM_LOOP_DESC)) begin
      if (state != STATE_CONFIG) begin
        $fatal(1, "Loop descriptor writes are legal only in CONFIG");
      end
      if ((cfg_tile_row != '0) || (cfg_tile_col != '0)) begin
        $fatal(1, "Loop descriptor is global and requires tile row=0 col=0");
      end
      if (cfg_word_idx != 2'd0) begin
        $fatal(1, "Loop descriptor requires cfg_word_idx=0");
      end
      if (cfg_addr > 10'd4) begin
        $fatal(1, "Loop descriptor address out of range: addr=%0d", cfg_addr);
      end
      if ((cfg_addr == 10'd4) && (cfg_wdata > 32'd1)) begin
        $fatal(1, "LOOP_COMMIT data must be 0 or 1: data=%0d", cfg_wdata);
      end
    end
  end
`endif

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      state <= STATE_CONFIG;
      kernel_pc <= '0;
      cycles_left <= '0;
      loop_prologue_shadow <= '0;
      loop_ii_shadow <= '0;
      loop_trip_count_shadow <= '0;
      loop_epilogue_shadow <= '0;
      loop_prologue_cycles <= '0;
      loop_ii_cycles <= '0;
      loop_trip_count <= '0;
      loop_epilogue_cycles <= '0;
      loop_committed <= 1'b0;
      loop_phase <= LOOP_PHASE_FLAT;
      loop_phase_count <= '0;
      loop_iterations_left <= '0;
      loop_iteration <= '0;
      loop_kernel_slot <= '0;
    end else begin
      if (loop_desc_cfg_accept) begin
        unique case (cfg_addr)
          10'd0: loop_prologue_shadow <= cfg_wdata;
          10'd1: loop_ii_shadow <= cfg_wdata;
          10'd2: loop_trip_count_shadow <= cfg_wdata;
          10'd3: loop_epilogue_shadow <= cfg_wdata;
          10'd4: begin
            if (cfg_wdata == 0) begin
              loop_committed <= 1'b0;
            end else if (loop_descriptor_valid(loop_prologue_shadow,
                                               loop_ii_shadow,
                                               loop_trip_count_shadow,
                                               loop_epilogue_shadow)) begin
              loop_prologue_cycles <= loop_prologue_shadow;
              loop_ii_cycles <= loop_ii_shadow;
              loop_trip_count <= loop_trip_count_shadow;
              loop_epilogue_cycles <= loop_epilogue_shadow;
              loop_committed <= 1'b1;
            end else begin
              loop_committed <= 1'b0;
`ifndef SYNTHESIS
              $fatal(1,
                     "Invalid loop descriptor: P=%0d II=%0d N=%0d E=%0d depth=%0d",
                     loop_prologue_shadow, loop_ii_shadow,
                     loop_trip_count_shadow, loop_epilogue_shadow,
                     cgra_pkg::CTRL_MEM_DEPTH);
`endif
            end
          end
          default: begin
          end
        endcase
      end

      unique case (state)
        STATE_CONFIG: begin
          kernel_pc <= '0;
          cycles_left <= '0;
          loop_phase <= LOOP_PHASE_FLAT;
          loop_phase_count <= '0;
          loop_iterations_left <= '0;
          loop_iteration <= '0;
          loop_kernel_slot <= '0;
          if (start) begin
            state <= STATE_RUN;
            if (loop_committed) begin
              cycles_left <= '0;
              loop_iterations_left <= loop_trip_count;
              if (loop_prologue_cycles != 0) begin
                loop_phase <= LOOP_PHASE_PROLOGUE;
                loop_phase_count <= loop_prologue_cycles;
                kernel_pc <= '0;
              end else begin
                loop_phase <= LOOP_PHASE_KERNEL;
                loop_kernel_slot <= '0;
                kernel_pc <= cgra_pkg::CTRL_PC_WIDTH'(loop_prologue_cycles);
              end
            end else begin
              kernel_pc <= '0;
              cycles_left <= run_cycles;
            end
          end
        end
        STATE_RUN: begin
          if (loop_committed) begin
            if (loop_last_cycle) begin
              state <= STATE_DONE;
              loop_phase_count <= '0;
              loop_iterations_left <= '0;
            end else begin
              unique case (loop_phase)
                LOOP_PHASE_PROLOGUE: begin
                  if (loop_phase_count <= 32'd1) begin
                    loop_phase <= LOOP_PHASE_KERNEL;
                    loop_phase_count <= '0;
                    loop_kernel_slot <= '0;
                    kernel_pc <= cgra_pkg::CTRL_PC_WIDTH'(loop_prologue_cycles);
                  end else begin
                    loop_phase_count <= loop_phase_count - 1'b1;
                    kernel_pc <= kernel_pc + 1'b1;
                  end
                end
                LOOP_PHASE_KERNEL: begin
                  if ((32'(loop_kernel_slot) + 32'd1) < loop_ii_cycles) begin
                    loop_kernel_slot <= loop_kernel_slot + 1'b1;
                    kernel_pc <= kernel_pc + 1'b1;
                  end else if (loop_iterations_left > 32'd1) begin
                    loop_iterations_left <= loop_iterations_left - 1'b1;
                    loop_iteration <= loop_iteration + 1'b1;
                    loop_kernel_slot <= '0;
                    kernel_pc <= cgra_pkg::CTRL_PC_WIDTH'(loop_prologue_cycles);
                  end else begin
                    loop_phase <= LOOP_PHASE_EPILOGUE;
                    loop_phase_count <= loop_epilogue_cycles;
                    loop_iteration <= loop_trip_count;
                    loop_kernel_slot <= '0;
                    kernel_pc <= cgra_pkg::CTRL_PC_WIDTH'(loop_prologue_cycles
                                                         + loop_ii_cycles);
                  end
                end
                LOOP_PHASE_EPILOGUE: begin
                  loop_phase_count <= loop_phase_count - 1'b1;
                  kernel_pc <= kernel_pc + 1'b1;
                end
                default: begin
                  state <= STATE_DONE;
                end
              endcase
            end
          end else if (cycles_left <= 1) begin
            state <= STATE_DONE;
            cycles_left <= '0;
          end else begin
            cycles_left <= cycles_left - 1'b1;
            kernel_pc <= kernel_pc + 1'b1;
          end
        end
        STATE_DONE: begin
          state <= STATE_CONFIG;
          kernel_pc <= '0;
          cycles_left <= '0;
          loop_phase <= LOOP_PHASE_FLAT;
          loop_phase_count <= '0;
          loop_iterations_left <= '0;
          loop_iteration <= '0;
          loop_kernel_slot <= '0;
        end
        default: begin
          state <= STATE_CONFIG;
          kernel_pc <= '0;
          cycles_left <= '0;
          loop_phase <= LOOP_PHASE_FLAT;
          loop_phase_count <= '0;
          loop_iterations_left <= '0;
          loop_iteration <= '0;
          loop_kernel_slot <= '0;
        end
      endcase
    end
  end
endmodule : cgra_top
