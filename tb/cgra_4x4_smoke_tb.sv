// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module cgra_4x4_smoke_tb(input logic clk);
  import cgra_pkg::*;

  localparam int ROWS = ARRAY_ROWS;
  localparam int COLS = ARRAY_COLS;
  localparam int TILES = ROWS * COLS;
  localparam int INIT_PCS = 4;
  localparam int INIT_CFG_CYCLES = TILES * INIT_PCS * CONTROL_WORD_CHUNKS;
  localparam int SPECIAL_CONTROLS = 6;
  localparam int SPECIAL_CFG_CYCLES = SPECIAL_CONTROLS * CONTROL_WORD_CHUNKS;
  localparam int CONST_CFG_START = 1 + INIT_CFG_CYCLES;
  localparam int SPECIAL_CFG_START = CONST_CFG_START + 2;
  localparam int START_CYCLE = SPECIAL_CFG_START + SPECIAL_CFG_CYCLES;

  logic rst_n;
  logic cfg_valid;
  logic cfg_ready;
  logic cfg_we;
  logic [1:0] cfg_mem_type;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_row;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_col;
  logic [SCRATCHPAD_ADDR_WIDTH-1:0] cfg_addr;
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
  logic [SCRATCHPAD_ADDR_WIDTH-1:0] init_pc;
  logic [1:0] init_chunk;
  logic [2:0] special_index;
  logic [1:0] special_chunk;
  tile_control_word_t next_ctrl;
  control_word_bits_t packed_ctrl;

  cgra_top dut (
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

  task automatic drive_cfg_int(input logic [1:0] mem_type,
                               input logic [CTRL_PC_WIDTH-1:0] row,
                               input logic [CTRL_PC_WIDTH-1:0] col,
                               input logic [SCRATCHPAD_ADDR_WIDTH-1:0] addr,
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

  task automatic build_special_control(input logic [2:0] which,
                                       output logic [CTRL_PC_WIDTH-1:0] row,
                                       output logic [CTRL_PC_WIDTH-1:0] col,
                                       output logic [SCRATCHPAD_ADDR_WIDTH-1:0] pc);
    begin
      clear_next_ctrl();
      row = 0;
      col = 0;
      pc = 0;
      unique case (which)
        0: begin
          row = 0;
          col = 0;
          pc = 0;
          next_ctrl.const_ctrl.const_addr = 4'h0;
          next_ctrl.data_route_ctrl.east.we = 1'b1;
          next_ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        end
        1: begin
          row = 0;
          col = 1;
          pc = 1;
          next_ctrl.data_route_ctrl.east.we = 1'b1;
          next_ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        end
        2: begin
          row = 0;
          col = 2;
          pc = 2;
          next_ctrl.data_route_ctrl.north.we = 1'b1;
          next_ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        end
        3: begin
          row = 3;
          col = 0;
          pc = 0;
          next_ctrl.const_ctrl.const_addr = 4'h0;
          next_ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
          next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_ZERO;
          next_ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_CONST_DATA;
        end
        4: begin
          row = 3;
          col = 0;
          pc = 1;
          next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
          next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_ZERO;
        end
        5: begin
          row = 3;
          col = 0;
          pc = 3;
          next_ctrl.data_route_ctrl.north.we = 1'b1;
          next_ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_LSU_LOAD_DATA;
        end
        default: begin
        end
      endcase
      packed_ctrl = pack_tile_control_word(next_ctrl);
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
    init_index = 0;
    init_tile = 0;
    init_pc = 0;
    init_chunk = 0;
    special_index = 0;
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
      init_pc = SCRATCHPAD_ADDR_WIDTH'((init_index / CONTROL_WORD_CHUNKS) % INIT_PCS);
      init_tile = init_index / (CONTROL_WORD_CHUNKS * INIT_PCS);
      drive_cfg_int(2'd0,
                    CTRL_PC_WIDTH'(init_tile / COLS),
                    CTRL_PC_WIDTH'(init_tile % COLS),
                    init_pc,
                    init_chunk,
                    32'h0000_0000);
    end else if (cycle == CONST_CFG_START) begin
      drive_cfg_int(1, 0, 0, 0, 0, 32'h1111_0001);
    end else if (cycle == CONST_CFG_START + 1) begin
      drive_cfg_int(1, 3, 0, 0, 0, 32'hcccc_0003);
    end else if ((cycle >= SPECIAL_CFG_START) && (cycle < START_CYCLE)) begin
      logic [CTRL_PC_WIDTH-1:0] special_row;
      logic [CTRL_PC_WIDTH-1:0] special_col;
      logic [SCRATCHPAD_ADDR_WIDTH-1:0] special_pc;
      special_index = 3'((cycle - SPECIAL_CFG_START) / CONTROL_WORD_CHUNKS);
      special_chunk = 2'((cycle - SPECIAL_CFG_START) % CONTROL_WORD_CHUNKS);
      build_special_control(special_index, special_row, special_col, special_pc);
      drive_cfg_int(0,
                    special_row,
                    special_col,
                    special_pc,
                    special_chunk,
                    packed_ctrl[special_chunk*32 +: 32]);
    end else if (cycle == START_CYCLE) begin
      run_cycles <= 8'd4;
      start <= 1'b1;
    end else if (cycle == START_CYCLE + 1) begin
      expect_bit("4x4 busy cycle 0", busy, 1'b1);
      expect_bit("4x4 cfg_ready during run", cfg_ready, 1'b0);
      expect_pc("4x4 pc cycle 0", kernel_pc, 8'd0);
    end else if (cycle == START_CYCLE + 2) begin
      expect_bit("4x4 busy cycle 1", busy, 1'b1);
      expect_pc("4x4 pc cycle 1", kernel_pc, 8'd1);
      expect_bit("4x4 one-hop east valid", east_data_we[0], 1'b1);
      expect_data("4x4 one-hop east data", east_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'h1111_0001);
    end else if (cycle == START_CYCLE + 3) begin
      expect_bit("4x4 busy cycle 2", busy, 1'b1);
      expect_pc("4x4 pc cycle 2", kernel_pc, 8'd2);
      expect_bit("4x4 two-hop intermediate valid", east_data_we[1], 1'b1);
      expect_data("4x4 two-hop intermediate data", east_data_out[1*DATA_WIDTH +: DATA_WIDTH], 32'h1111_0001);
    end else if (cycle == START_CYCLE + 4) begin
      expect_bit("4x4 busy cycle 3", busy, 1'b1);
      expect_pc("4x4 pc cycle 3", kernel_pc, 8'd3);
      expect_bit("4x4 two-hop final valid", north_data_we[2], 1'b1);
      expect_data("4x4 two-hop final data", north_data_out[2*DATA_WIDTH +: DATA_WIDTH], 32'h1111_0001);
    end else if (cycle == START_CYCLE + 5) begin
      expect_bit("4x4 done after run", done, 1'b1);
      expect_bit("4x4 edge LSU route valid", north_data_we[12], 1'b1);
      expect_data("4x4 edge LSU route data", north_data_out[12*DATA_WIDTH +: DATA_WIDTH], 32'hcccc_0003);
    end else if (cycle == START_CYCLE + 6) begin
      expect_bit("4x4 done pulse clears", done, 1'b0);
      expect_bit("4x4 cfg_ready after run", cfg_ready, 1'b1);
      $display("CGRA 4x4 smoke test passed");
      $finish;
    end else if (cycle > START_CYCLE + 8) begin
      $fatal(1, "CGRA 4x4 smoke test timed out");
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : cgra_4x4_smoke_tb
