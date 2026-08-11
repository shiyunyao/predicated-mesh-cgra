// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module runtime_corner_tb(input logic clk);
  import cgra_pkg::*;

  localparam int ROWS = 2;
  localparam int COLS = 2;
  localparam int TILES = ROWS * COLS;
  localparam logic [TILES-1:0] LEFT_COLUMN_LSU_MASK = 4'b0101;

  logic rst_n;
  logic cfg_valid;
  logic cfg_ready;
  logic cfg_we;
  logic [1:0] cfg_mem_type;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_row;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_col;
  logic [SCRATCH_ADDR_WIDTH-1:0] cfg_addr;
  logic [1:0] cfg_word_idx;
  logic [31:0] cfg_wdata;
  logic start;
  logic [CTRL_PC_WIDTH-1:0] run_cycles;
  logic busy;
  logic done;
  logic [CTRL_PC_WIDTH-1:0] kernel_pc;
  logic [TILES-1:0] north_data_we;
  logic [TILES*DATA_WIDTH-1:0] north_data_out;
  logic [TILES-1:0] south_data_we;
  logic [TILES*DATA_WIDTH-1:0] south_data_out;
  logic [TILES-1:0] east_data_we;
  logic [TILES*DATA_WIDTH-1:0] east_data_out;
  logic [TILES-1:0] west_data_we;
  logic [TILES*DATA_WIDTH-1:0] west_data_out;
  logic out_of_range_tile_mode;
  logic zero_cycles_unresolved_mode;
  logic unused_outputs;
  int cycle;
  tile_control_word_t next_ctrl;
  control_word_bits_t packed_ctrl;

  cgra_top #(
    .ROWS(ROWS),
    .COLS(COLS),
    .HAS_LSU_MASK(LEFT_COLUMN_LSU_MASK)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .cfg_valid(cfg_valid),
    .cfg_ready(cfg_ready),
    .cfg_we(cfg_we),
    .cfg_mem_type(cfg_mem_type),
    .cfg_tile_row(cfg_tile_row),
    .cfg_tile_col(cfg_tile_col),
    .cfg_addr(cfg_addr),
    .cfg_word_idx(cfg_word_idx),
    .cfg_wdata(cfg_wdata),
    .start(start),
    .run_cycles(run_cycles),
    .busy(busy),
    .done(done),
    .kernel_pc(kernel_pc),
    .north_data_we(north_data_we),
    .north_data_out(north_data_out),
    .south_data_we(south_data_we),
    .south_data_out(south_data_out),
    .east_data_we(east_data_we),
    .east_data_out(east_data_out),
    .west_data_we(west_data_we),
    .west_data_out(west_data_out)
  );

  assign unused_outputs = (|north_data_we)
                          ^ (^north_data_out)
                          ^ (|south_data_we)
                          ^ (^south_data_out)
                          ^ (|east_data_we)
                          ^ (^east_data_out)
                          ^ (|west_data_we)
                          ^ (^west_data_out);

  always_comb begin
    if (unused_outputs) begin
    end
  end

/* verilator lint_off BLKSEQ */
  task automatic clear_next_ctrl;
    begin
      next_ctrl = '0;
      next_ctrl.exec_ctrl.op = OP_NOP;
      next_ctrl.lsu_ctrl.lsu_op = LSU_OP_NONE;
    end
  endtask

  task automatic clear_cfg;
    begin
      cfg_valid = 1'b0;
      cfg_we = 1'b0;
      cfg_mem_type = '0;
      cfg_tile_row = '0;
      cfg_tile_col = '0;
      cfg_addr = '0;
      cfg_word_idx = '0;
      cfg_wdata = '0;
    end
  endtask

  task automatic drive_cfg(input logic [1:0] mem_type,
                           input logic [CTRL_PC_WIDTH-1:0] row,
                           input logic [CTRL_PC_WIDTH-1:0] col,
                           input logic [SCRATCH_ADDR_WIDTH-1:0] addr,
                           input logic [1:0] word_idx,
                           input logic [31:0] data);
    begin
      cfg_valid = 1'b1;
      cfg_we = 1'b1;
      cfg_mem_type = mem_type;
      cfg_tile_row = row;
      cfg_tile_col = col;
      cfg_addr = addr;
      cfg_word_idx = word_idx;
      cfg_wdata = data;
    end
  endtask

  task automatic drive_control_chunk(input logic [SCRATCH_ADDR_WIDTH-1:0] addr,
                                     input logic [1:0] word_idx);
    begin
      drive_cfg(2'd0, 8'd0, 8'd0, addr, word_idx, packed_ctrl[word_idx * 32 +: 32]);
    end
  endtask
/* verilator lint_on BLKSEQ */

  task automatic expect_data(input string name,
                             input logic [DATA_WIDTH-1:0] actual,
                             input logic [DATA_WIDTH-1:0] expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected 0x%08h, got 0x%08h", name, expected, actual);
        $finish;
      end
    end
  endtask

  task automatic expect_bit(input string name,
                            input logic actual,
                            input logic expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected %0d, got %0d", name, expected, actual);
        $finish;
      end
    end
  endtask

  task automatic expect_pc(input string name,
                           input logic [CTRL_PC_WIDTH-1:0] actual,
                           input logic [CTRL_PC_WIDTH-1:0] expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected %0d, got %0d", name, expected, actual);
        $finish;
      end
    end
  endtask

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    clear_cfg();
    start = 1'b0;
    run_cycles = '0;
    clear_next_ctrl();
    packed_ctrl = '0;
    out_of_range_tile_mode = ($test$plusargs("OUT_OF_RANGE_TILE") != 0);
    zero_cycles_unresolved_mode = ($test$plusargs("ZERO_CYCLES_UNRESOLVED") != 0);
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    clear_cfg();
    start <= 1'b0;

    unique case (cycle)
      0: begin
        rst_n <= 1'b0;
      end
      1: begin
        expect_bit("config ready after reset", cfg_ready, 1'b1);
        if (out_of_range_tile_mode) begin
          drive_cfg(2'd1, 8'(ROWS), 8'd0, 10'd0, 2'd0, 32'h1111_2222);
        end else begin
          drive_cfg(2'd1, 8'd0, 8'd0, 10'd0, 2'd0, 32'hcafe_0001);
        end
      end
      2: begin
        if (out_of_range_tile_mode) begin
          $fatal(1, "OUT_OF_RANGE_TILE scenario did not trip top assertion");
        end
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h0;
        next_ctrl.data_route_ctrl.north.we = 1'b1;
        next_ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_CONST_DATA;
        packed_ctrl = pack_tile_control_word(next_ctrl);
        drive_control_chunk(10'd0, 2'd0);
      end
      3: begin
        drive_control_chunk(10'd0, 2'd1);
      end
      4: begin
        drive_control_chunk(10'd0, 2'd2);
      end
      5: begin
        drive_control_chunk(10'd0, 2'd3);
      end
      6: begin
        run_cycles <= zero_cycles_unresolved_mode ? 8'd0 : 8'd1;
        start <= 1'b1;
      end
      7: begin
        if (zero_cycles_unresolved_mode && busy) begin
          $fatal(1, "run_cycles=0 policy unresolved: current RTL enters RUN instead of documented zero-cycle behavior");
        end
        expect_bit("busy during first positive-cycle run", busy, 1'b1);
        expect_bit("cfg_ready low during RUN", cfg_ready, 1'b0);
        expect_pc("kernel_pc during first run", kernel_pc, 8'd0);
        drive_cfg(2'd1, 8'd0, 8'd0, 10'd0, 2'd0, 32'hdead_0002);
      end
      8: begin
        expect_bit("done after first one-cycle run", done, 1'b1);
        expect_bit("busy clear after first run", busy, 1'b0);
        expect_bit("cfg_ready high after first run", cfg_ready, 1'b1);
        expect_bit("first route valid", north_data_we[0], 1'b1);
        expect_data("first route uses original const", north_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'hcafe_0001);
      end
      9: begin
        expect_bit("done pulse clears after first run", done, 1'b0);
        expect_bit("config ready between runs", cfg_ready, 1'b1);
      end
      10: begin
        run_cycles <= 8'd1;
        start <= 1'b1;
      end
      11: begin
        expect_bit("busy during second run", busy, 1'b1);
        expect_bit("cfg_ready low during second RUN", cfg_ready, 1'b0);
        expect_pc("kernel_pc during second run", kernel_pc, 8'd0);
      end
      12: begin
        expect_bit("done after second one-cycle run", done, 1'b1);
        expect_bit("second route valid", north_data_we[0], 1'b1);
        expect_data("RUN-time config write had no effect", north_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'hcafe_0001);
        $display("Runtime corner test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Runtime corner test timed out");
      end
    endcase
  end
/* verilator lint_on BLKSEQ */
endmodule : runtime_corner_tb
