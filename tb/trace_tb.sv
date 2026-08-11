// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module trace_tb(input logic clk);
  import cgra_pkg::*;

  localparam int ROWS = 2;
  localparam int COLS = 2;
  localparam int TILES = ROWS * COLS;
  localparam int INIT_PCS = 2;
  localparam int INIT_CFG_CYCLES = TILES * INIT_PCS * CONTROL_WORD_CHUNKS;
  localparam int CONST_CFG_START = 1 + INIT_CFG_CYCLES;
  localparam int SPECIAL_CFG_START = CONST_CFG_START + 1;
  localparam int START_CYCLE = SPECIAL_CFG_START + CONTROL_WORD_CHUNKS;

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
  logic unused_outputs;
  int cycle;
  int init_index;
  int init_tile;
  logic [SCRATCH_ADDR_WIDTH-1:0] init_pc;
  logic [1:0] init_chunk;
  logic [1:0] special_chunk;
  tile_control_word_t next_ctrl;
  control_word_bits_t packed_ctrl;

  // T018 chooses CSV trace output. The Makefile trace check validates the
  // header and the expected one-record-per-tile-per-cycle row count.
  cgra_top #(
    .ROWS(ROWS),
    .COLS(COLS),
    .HAS_LSU_MASK(4'b0001)
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

  assign unused_outputs = cfg_ready
                          ^ (|north_data_we)
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

  task automatic drive_cfg_int(input logic [1:0] mem_type,
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
/* verilator lint_on BLKSEQ */

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
    init_index = 0;
    init_tile = 0;
    init_pc = 0;
    init_chunk = 0;
    special_chunk = 0;
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    clear_cfg();
    start <= 1'b0;

    if (cycle == 0) begin
      rst_n <= 1'b0;
    end else if ((cycle >= 1) && (cycle < CONST_CFG_START)) begin
      init_index = cycle - 1;
      init_chunk = 2'(init_index % CONTROL_WORD_CHUNKS);
      init_pc = SCRATCH_ADDR_WIDTH'((init_index / CONTROL_WORD_CHUNKS) % INIT_PCS);
      init_tile = init_index / (CONTROL_WORD_CHUNKS * INIT_PCS);
      drive_cfg_int(2'd0,
                    CTRL_PC_WIDTH'(init_tile / COLS),
                    CTRL_PC_WIDTH'(init_tile % COLS),
                    init_pc,
                    init_chunk,
                    32'h0000_0000);
    end else if (cycle == CONST_CFG_START) begin
      drive_cfg_int(1, 0, 0, 0, 0, 32'h1234_5678);
    end else if ((cycle >= SPECIAL_CFG_START) && (cycle < START_CYCLE)) begin
      clear_next_ctrl();
      next_ctrl.const_ctrl.const_addr = 4'h0;
      next_ctrl.exec_ctrl.op = OP_PASS;
      next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_CONST_DATA;
      next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
      next_ctrl.data_rf_ctrl.data_w0_addr = 4'h2;
      next_ctrl.data_route_ctrl.east.we = 1'b1;
      next_ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_FU_DATA;
      packed_ctrl = pack_tile_control_word(next_ctrl);
      special_chunk = 2'((cycle - SPECIAL_CFG_START) % CONTROL_WORD_CHUNKS);
      drive_cfg_int(0,
                    0,
                    0,
                    0,
                    special_chunk,
                    packed_ctrl[special_chunk*32 +: 32]);
    end else if (cycle == START_CYCLE) begin
      run_cycles <= 8'd2;
      start <= 1'b1;
    end else if (cycle == START_CYCLE + 1) begin
      expect_bit("trace busy cycle 0", busy, 1'b1);
      expect_pc("trace pc cycle 0", kernel_pc, 8'd0);
    end else if (cycle == START_CYCLE + 2) begin
      expect_bit("trace busy cycle 1", busy, 1'b1);
      expect_pc("trace pc cycle 1", kernel_pc, 8'd1);
    end else if (cycle == START_CYCLE + 3) begin
      expect_bit("trace done", done, 1'b1);
    end else if (cycle == START_CYCLE + 4) begin
      expect_bit("trace done pulse clears", done, 1'b0);
      $display("Trace test run completed");
      $finish;
    end else if (cycle > START_CYCLE + 6) begin
      $fatal(1, "Trace test timed out");
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : trace_tb
