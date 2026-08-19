// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module modulo_loop_reuse_tb(input logic clk);
  import cgra_pkg::*;

  localparam int TILES = 16;
  localparam logic [TILES-1:0] HAS_LSU_MASK = 16'h1111;
  localparam logic [31:0] NOP_W0 = 32'h001de200;
  localparam logic [31:0] NOP_W1 = 32'hc0080000;
  localparam logic [31:0] NOP_W2 = 32'h00000001;
  localparam logic [31:0] NOP_W3 = 32'h1c880000;

  logic rst_n, cfg_valid, cfg_ready, cfg_we, start, busy, done;
  logic [1:0] cfg_mem_type, cfg_word_idx;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_row, cfg_tile_col, kernel_pc;
  logic [SCRATCH_ADDR_WIDTH-1:0] cfg_addr;
  logic [31:0] cfg_wdata;
  logic [TILES-1:0] north_data_we, south_data_we, east_data_we, west_data_we;
  logic [TILES*DATA_WIDTH-1:0] north_data_out, south_data_out, east_data_out, west_data_out;
  logic unused_outputs;
  int cycle;

  cgra_top #(.ROWS(4), .COLS(4), .HAS_LSU_MASK(HAS_LSU_MASK)) dut (
    .clk(clk), .rst_n(rst_n), .cfg_valid(cfg_valid), .cfg_ready(cfg_ready),
    .cfg_we(cfg_we), .cfg_mem_type(cfg_mem_type), .cfg_tile_row(cfg_tile_row),
    .cfg_tile_col(cfg_tile_col), .cfg_addr(cfg_addr), .cfg_word_idx(cfg_word_idx),
    .cfg_wdata(cfg_wdata), .start(start), .run_cycles('0), .busy(busy), .done(done),
    .kernel_pc(kernel_pc), .north_data_we(north_data_we), .north_data_out(north_data_out),
    .south_data_we(south_data_we), .south_data_out(south_data_out),
    .east_data_we(east_data_we), .east_data_out(east_data_out),
    .west_data_we(west_data_we), .west_data_out(west_data_out)
  );

  assign unused_outputs = (|north_data_we) ^ (^north_data_out)
                        ^ (|south_data_we) ^ (^south_data_out)
                        ^ (|east_data_we) ^ (^east_data_out)
                        ^ (|west_data_we) ^ (^west_data_out);

  task automatic clear_cfg;
    begin
      cfg_valid = 1'b0; cfg_we = 1'b0; cfg_mem_type = '0;
      cfg_tile_row = '0; cfg_tile_col = '0; cfg_addr = '0;
      cfg_word_idx = '0; cfg_wdata = '0;
    end
  endtask

  task automatic drive_cfg(input logic [1:0] mem_type,
                           input logic [CTRL_PC_WIDTH-1:0] row,
                           input logic [CTRL_PC_WIDTH-1:0] col,
                           input logic [SCRATCH_ADDR_WIDTH-1:0] addr,
                           input logic [1:0] word_idx, input logic [31:0] data);
    begin
      cfg_valid = 1'b1; cfg_we = 1'b1; cfg_mem_type = mem_type;
      cfg_tile_row = row; cfg_tile_col = col;
      cfg_addr = addr; cfg_word_idx = word_idx; cfg_wdata = data;
    end
  endtask

  initial begin
    cycle = 0; rst_n = 1'b0; start = 1'b0; clear_cfg();
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    clear_cfg();
    start <= 1'b0;
    unique case (cycle)
      0: rst_n <= 1'b0;
      1, 2, 3, 4: begin
        if (!cfg_ready) $fatal(1, "config not ready during tile0 image");
        drive_cfg(2'd0, '0, '0, '0, 2'(cycle - 1),
                  (cycle - 1 == 0) ? NOP_W0 : (cycle - 1 == 1) ? NOP_W1 : (cycle - 1 == 2) ? NOP_W2 : NOP_W3);
      end
      5, 6, 7, 8: begin
        if (!cfg_ready) $fatal(1, "config not ready during tile1 image");
        drive_cfg(2'd0, '0, CTRL_PC_WIDTH'(1), '0, 2'(cycle - 5),
                  (cycle - 5 == 0) ? NOP_W0 : (cycle - 5 == 1) ? NOP_W1 : (cycle - 5 == 2) ? NOP_W2 : NOP_W3);
      end
      9: drive_cfg(2'd3, 0, 0, 0, 2'd0, 32'd0);
      10: drive_cfg(2'd3, 0, 0, 1, 2'd0, 32'd1);
      11: drive_cfg(2'd3, 0, 0, 2, 2'd0, 32'd2);
      12: drive_cfg(2'd3, 0, 0, 3, 2'd0, 32'd0);
      13: drive_cfg(2'd3, 0, 0, 4, 2'd0, 32'd1);
      14: start <= 1'b1;
      15, 16: begin
        if (!busy || kernel_pc !== 8'd0) $fatal(1, "first committed loop run mismatch at cycle %0d", cycle);
      end
      17: if (!done) $fatal(1, "first loop DONE missing");
      18: start <= 1'b1;
      19, 20: begin
        if (!busy || kernel_pc !== 8'd0) $fatal(1, "reused committed loop run mismatch at cycle %0d", cycle);
      end
      21: if (!done) $fatal(1, "reused loop DONE missing");
      22: begin
        if (done) $fatal(1, "DONE pulse did not clear after descriptor reuse");
        $display("MODULO_LOOP_DESCRIPTOR_REUSE_PASS");
        $finish;
      end
      default: if (cycle > 24) $fatal(1, "descriptor reuse test timed out");
    endcase
  end
/* verilator lint_on BLKSEQ */
endmodule : modulo_loop_reuse_tb
