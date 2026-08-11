// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module random_dfg_tb(input logic clk);
  import cgra_pkg::*;

  localparam int SEED = 7;
  localparam int REQUESTED_STEPS = 21;
  localparam int ROWS = 1;
  localparam int COLS = 2;
  localparam int TILES = ROWS * COLS;
  localparam int RUN_CYCLES = 21;
  localparam int CONST_COUNT = 3;
  localparam int CONST_CFG_START = 1;
  localparam int CTRL_CFG_START = CONST_CFG_START + CONST_COUNT;
  localparam int START_CYCLE = CTRL_CFG_START + (RUN_CYCLES * CONTROL_WORD_CHUNKS);

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
  int ctrl_index;
  int observed_pc;

  cgra_top #(
    .ROWS(ROWS),
    .COLS(COLS),
    .HAS_LSU_MASK(2'b00)
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

  task automatic drive_const(input int index);
    begin
      unique case (index)
        0: drive_cfg_int(1, 0, 0, SCRATCH_ADDR_WIDTH'(0), 0, 32'h52e6b438);
        1: drive_cfg_int(1, 0, 0, SCRATCH_ADDR_WIDTH'(1), 0, 32'hf2a74de4);
        2: drive_cfg_int(1, 0, 0, SCRATCH_ADDR_WIDTH'(2), 0, 32'h269e0d37);
        default: clear_cfg();
      endcase
    end
  endtask

  task automatic drive_control(input int index);
    begin
      unique case (index)
        0: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(0), 2'(0), 32'h001de181);
        1: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(0), 2'(1), 32'hc0080040);
        2: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(0), 2'(2), 32'h0000b001);
        3: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(0), 2'(3), 32'h1c880000);
        4: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(1), 2'(0), 32'h001de181);
        5: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(1), 2'(1), 32'hc00800c0);
        6: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(1), 2'(2), 32'h0000b001);
        7: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(1), 2'(3), 32'h1c880400);
        8: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(2), 2'(0), 32'h001de181);
        9: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(2), 2'(1), 32'hc0080140);
        10: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(2), 2'(2), 32'h0000b001);
        11: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(2), 2'(3), 32'h1c880800);
        12: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(3), 2'(0), 32'h001de200);
        13: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(3), 2'(1), 32'h82080000);
        14: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(3), 2'(2), 32'h00000001);
        15: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(3), 2'(3), 32'h1c880000);
        16: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(4), 2'(0), 32'h001de200);
        17: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(4), 2'(1), 32'hc6080000);
        18: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(4), 2'(2), 32'h00000001);
        19: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(4), 2'(3), 32'h1c880000);
        20: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(5), 2'(0), 32'h041dc411);
        21: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(5), 2'(1), 32'hc0580000);
        22: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(5), 2'(2), 32'h00000001);
        23: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(5), 2'(3), 32'h1c880000);
        24: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(6), 2'(0), 32'h841c040a);
        25: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(6), 2'(1), 32'hc0080140);
        26: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(6), 2'(2), 32'h0000b001);
        27: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(6), 2'(3), 32'h1c880000);
        28: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(7), 2'(0), 32'h041dc410);
        29: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(7), 2'(1), 32'hc0580000);
        30: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(7), 2'(2), 32'h00000001);
        31: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(7), 2'(3), 32'h1c880000);
        32: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(8), 2'(0), 32'h841c040a);
        33: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(8), 2'(1), 32'hc0080140);
        34: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(8), 2'(2), 32'h0000b001);
        35: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(8), 2'(3), 32'h1c880000);
        36: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(9), 2'(0), 32'h041dc412);
        37: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(9), 2'(1), 32'hc0580000);
        38: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(9), 2'(2), 32'h00000001);
        39: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(9), 2'(3), 32'h1c880000);
        40: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(10), 2'(0), 32'h841c040a);
        41: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(10), 2'(1), 32'hc0080140);
        42: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(10), 2'(2), 32'h0000b001);
        43: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(10), 2'(3), 32'h1c880000);
        44: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(11), 2'(0), 32'h041dc413);
        45: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(11), 2'(1), 32'hc0580000);
        46: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(11), 2'(2), 32'h00000001);
        47: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(11), 2'(3), 32'h1c880000);
        48: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(12), 2'(0), 32'h841c040a);
        49: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(12), 2'(1), 32'hc0080140);
        50: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(12), 2'(2), 32'h0000b001);
        51: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(12), 2'(3), 32'h1c880000);
        52: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(13), 2'(0), 32'h00042221);
        53: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(13), 2'(1), 32'hc0580004);
        54: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(13), 2'(2), 32'h00000001);
        55: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(13), 2'(3), 32'h1c880000);
        56: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(14), 2'(0), 32'h841c040a);
        57: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(14), 2'(1), 32'hc0080140);
        58: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(14), 2'(2), 32'h0000b001);
        59: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(14), 2'(3), 32'h1c880000);
        60: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(15), 2'(0), 32'h00042223);
        61: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(15), 2'(1), 32'hc0580004);
        62: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(15), 2'(2), 32'h00000001);
        63: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(15), 2'(3), 32'h1c880000);
        64: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(16), 2'(0), 32'h841c040a);
        65: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(16), 2'(1), 32'hc0080140);
        66: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(16), 2'(2), 32'h0000b001);
        67: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(16), 2'(3), 32'h1c880000);
        68: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(17), 2'(0), 32'h00042222);
        69: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(17), 2'(1), 32'hc0580004);
        70: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(17), 2'(2), 32'h00000001);
        71: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(17), 2'(3), 32'h1c880000);
        72: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(18), 2'(0), 32'h841c040a);
        73: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(18), 2'(1), 32'hc0080140);
        74: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(18), 2'(2), 32'h0000b001);
        75: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(18), 2'(3), 32'h1c880000);
        76: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(19), 2'(0), 32'h00042220);
        77: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(19), 2'(1), 32'hc0580004);
        78: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(19), 2'(2), 32'h00000001);
        79: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(19), 2'(3), 32'h1c880000);
        80: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(20), 2'(0), 32'h841c040a);
        81: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(20), 2'(1), 32'hc0080140);
        82: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(20), 2'(2), 32'h0000b001);
        83: drive_cfg_int(0, 0, 0, SCRATCH_ADDR_WIDTH'(20), 2'(3), 32'h1c880000);
        default: clear_cfg();
      endcase
    end
  endtask
/* verilator lint_on BLKSEQ */

  function automatic logic expected_data_valid(input int pc);
    begin
      expected_data_valid = 1'b0;
      unique case (pc)
        0: expected_data_valid = 1'b1;
        1: expected_data_valid = 1'b1;
        2: expected_data_valid = 1'b1;
        3: expected_data_valid = 1'b0;
        4: expected_data_valid = 1'b0;
        5: expected_data_valid = 1'b0;
        6: expected_data_valid = 1'b1;
        7: expected_data_valid = 1'b0;
        8: expected_data_valid = 1'b1;
        9: expected_data_valid = 1'b0;
        10: expected_data_valid = 1'b1;
        11: expected_data_valid = 1'b0;
        12: expected_data_valid = 1'b1;
        13: expected_data_valid = 1'b0;
        14: expected_data_valid = 1'b1;
        15: expected_data_valid = 1'b0;
        16: expected_data_valid = 1'b1;
        17: expected_data_valid = 1'b0;
        18: expected_data_valid = 1'b1;
        19: expected_data_valid = 1'b0;
        20: expected_data_valid = 1'b1;
        default: expected_data_valid = 1'b0;
      endcase
    end
  endfunction

  function automatic logic [DATA_WIDTH-1:0] expected_value(input int pc);
    logic [DATA_WIDTH-1:0] expected;
    begin
      expected = '0;
      unique case (pc)
        0: expected = 32'h52e6b438;
        1: expected = 32'hf2a74de4;
        2: expected = 32'h269e0d37;
        3: expected = 32'h00000000;
        4: expected = 32'h00000000;
        5: expected = 32'h00000000;
        6: expected = 32'h52e6b438;
        7: expected = 32'h00000000;
        8: expected = 32'hf2a74de4;
        9: expected = 32'h00000000;
        10: expected = 32'h52e6b438;
        11: expected = 32'h00000000;
        12: expected = 32'h52e6b438;
        13: expected = 32'h00000000;
        14: expected = 32'hf2a74de4;
        15: expected = 32'h00000000;
        16: expected = 32'h52e6b438;
        17: expected = 32'h00000000;
        18: expected = 32'hf2a74de4;
        19: expected = 32'h00000000;
        20: expected = 32'h52e6b438;
        default: expected = '0;
      endcase
      expected_value = expected;
    end
  endfunction

  task automatic expect_data(input string name,
                             input logic [DATA_WIDTH-1:0] actual,
                             input logic [DATA_WIDTH-1:0] expected);
    begin
      if (actual !== expected) begin
        $error("%s seed=%0d requested_steps=%0d: expected 0x%08h, got 0x%08h", name, SEED, REQUESTED_STEPS, expected, actual);
        $finish;
      end
    end
  endtask

  task automatic expect_bit(input string name,
                            input logic actual,
                            input logic expected);
    begin
      if (actual !== expected) begin
        $error("%s seed=%0d requested_steps=%0d: expected %0d, got %0d", name, SEED, REQUESTED_STEPS, expected, actual);
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
    ctrl_index = 0;
    observed_pc = 0;
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    clear_cfg();
    start <= 1'b0;

    if (cycle == 0) begin
      rst_n <= 1'b0;
    end else if ((cycle >= CONST_CFG_START) && (cycle < CTRL_CFG_START)) begin
      drive_const(cycle - CONST_CFG_START);
    end else if ((cycle >= CTRL_CFG_START) && (cycle < START_CYCLE)) begin
      ctrl_index = cycle - CTRL_CFG_START;
      drive_control(ctrl_index);
    end else if (cycle == START_CYCLE) begin
      run_cycles <= CTRL_PC_WIDTH'(RUN_CYCLES);
      start <= 1'b1;
    end else if ((cycle >= START_CYCLE + 1) && (cycle <= START_CYCLE + RUN_CYCLES)) begin
      expect_bit("random DFG busy", busy, 1'b1);
      if (kernel_pc !== CTRL_PC_WIDTH'(cycle - START_CYCLE - 1)) begin
        $error("random DFG seed=%0d requested_steps=%0d: expected pc %0d, got %0d", SEED,
               REQUESTED_STEPS, cycle - START_CYCLE - 1, kernel_pc);
        $finish;
      end
      if (cycle >= START_CYCLE + 2) begin
        observed_pc = cycle - START_CYCLE - 2;
        expect_bit("random DFG east valid", east_data_we[0], expected_data_valid(observed_pc));
        if (expected_data_valid(observed_pc)) begin
          expect_data("random DFG east data", east_data_out[0*DATA_WIDTH +: DATA_WIDTH],
                      expected_value(observed_pc));
        end
      end
    end else if (cycle == START_CYCLE + RUN_CYCLES + 1) begin
      expect_bit("random DFG done", done, 1'b1);
      expect_bit("random DFG final east valid", east_data_we[0], expected_data_valid(RUN_CYCLES - 1));
      if (expected_data_valid(RUN_CYCLES - 1)) begin
        expect_data("random DFG final east data", east_data_out[0*DATA_WIDTH +: DATA_WIDTH],
                    expected_value(RUN_CYCLES - 1));
      end
    end else if (cycle == START_CYCLE + RUN_CYCLES + 2) begin
      expect_bit("random DFG done clears", done, 1'b0);
      $display("Random DFG test passed seed=%0d requested_steps=%0d run_cycles=%0d", SEED, REQUESTED_STEPS, RUN_CYCLES);
      $finish;
    end else if (cycle > START_CYCLE + RUN_CYCLES + 4) begin
      $fatal(1, "Random DFG test timed out seed=%0d", SEED);
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : random_dfg_tb
