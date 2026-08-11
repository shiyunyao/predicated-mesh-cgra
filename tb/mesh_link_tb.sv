// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module mesh_link_tb(input logic clk);
  import cgra_pkg::*;

  localparam int H_TILES = 2;
  localparam int V_TILES = 2;

  logic rst_n;
  logic [H_TILES*CONTROL_WORD_PHYSICAL_WIDTH-1:0] h_control_words;
  logic [H_TILES-1:0] h_const_cfg_we;
  logic [H_TILES*CONST_ADDR_WIDTH-1:0] h_const_cfg_addr;
  logic [H_TILES*DATA_WIDTH-1:0] h_const_cfg_wdata;
  logic [H_TILES-1:0] h_scratch_cfg_we;
  logic [H_TILES*SCRATCH_ADDR_WIDTH-1:0] h_scratch_cfg_addr;
  logic [H_TILES*DATA_WIDTH-1:0] h_scratch_cfg_wdata;
  logic [H_TILES-1:0] h_north_data_we;
  logic [H_TILES*DATA_WIDTH-1:0] h_north_data_out;
  logic [H_TILES-1:0] h_south_data_we;
  logic [H_TILES*DATA_WIDTH-1:0] h_south_data_out;
  logic [H_TILES-1:0] h_east_data_we;
  logic [H_TILES*DATA_WIDTH-1:0] h_east_data_out;
  logic [H_TILES-1:0] h_west_data_we;
  logic [H_TILES*DATA_WIDTH-1:0] h_west_data_out;
  logic [H_TILES-1:0] h_north_pred_we;
  logic [H_TILES*PRED_WIDTH-1:0] h_north_pred_out;
  logic [H_TILES-1:0] h_south_pred_we;
  logic [H_TILES*PRED_WIDTH-1:0] h_south_pred_out;
  logic [H_TILES-1:0] h_east_pred_we;
  logic [H_TILES*PRED_WIDTH-1:0] h_east_pred_out;
  logic [H_TILES-1:0] h_west_pred_we;
  logic [H_TILES*PRED_WIDTH-1:0] h_west_pred_out;

  logic [V_TILES*CONTROL_WORD_PHYSICAL_WIDTH-1:0] v_control_words;
  logic [V_TILES-1:0] v_const_cfg_we;
  logic [V_TILES*CONST_ADDR_WIDTH-1:0] v_const_cfg_addr;
  logic [V_TILES*DATA_WIDTH-1:0] v_const_cfg_wdata;
  logic [V_TILES-1:0] v_scratch_cfg_we;
  logic [V_TILES*SCRATCH_ADDR_WIDTH-1:0] v_scratch_cfg_addr;
  logic [V_TILES*DATA_WIDTH-1:0] v_scratch_cfg_wdata;
  logic [V_TILES-1:0] v_north_data_we;
  logic [V_TILES*DATA_WIDTH-1:0] v_north_data_out;
  logic [V_TILES-1:0] v_south_data_we;
  logic [V_TILES*DATA_WIDTH-1:0] v_south_data_out;
  logic [V_TILES-1:0] v_east_data_we;
  logic [V_TILES*DATA_WIDTH-1:0] v_east_data_out;
  logic [V_TILES-1:0] v_west_data_we;
  logic [V_TILES*DATA_WIDTH-1:0] v_west_data_out;
  logic [V_TILES-1:0] v_north_pred_we;
  logic [V_TILES*PRED_WIDTH-1:0] v_north_pred_out;
  logic [V_TILES-1:0] v_south_pred_we;
  logic [V_TILES*PRED_WIDTH-1:0] v_south_pred_out;
  logic [V_TILES-1:0] v_east_pred_we;
  logic [V_TILES*PRED_WIDTH-1:0] v_east_pred_out;
  logic [V_TILES-1:0] v_west_pred_we;
  logic [V_TILES*PRED_WIDTH-1:0] v_west_pred_out;

  int cycle;
  tile_control_word_t ctrl;
  logic unused_outputs;

  mesh #(
    .ROWS(1),
    .COLS(2)
  ) h_mesh (
    .clk(clk),
    .rst_n(rst_n),
    .control_words(h_control_words),
    .const_cfg_we(h_const_cfg_we),
    .const_cfg_addr(h_const_cfg_addr),
    .const_cfg_wdata(h_const_cfg_wdata),
    .scratch_cfg_we(h_scratch_cfg_we),
    .scratch_cfg_addr(h_scratch_cfg_addr),
    .scratch_cfg_wdata(h_scratch_cfg_wdata),
    .north_data_we(h_north_data_we),
    .north_data_out(h_north_data_out),
    .south_data_we(h_south_data_we),
    .south_data_out(h_south_data_out),
    .east_data_we(h_east_data_we),
    .east_data_out(h_east_data_out),
    .west_data_we(h_west_data_we),
    .west_data_out(h_west_data_out),
    .north_pred_we(h_north_pred_we),
    .north_pred_out(h_north_pred_out),
    .south_pred_we(h_south_pred_we),
    .south_pred_out(h_south_pred_out),
    .east_pred_we(h_east_pred_we),
    .east_pred_out(h_east_pred_out),
    .west_pred_we(h_west_pred_we),
    .west_pred_out(h_west_pred_out)
  );

  mesh #(
    .ROWS(2),
    .COLS(1)
  ) v_mesh (
    .clk(clk),
    .rst_n(rst_n),
    .control_words(v_control_words),
    .const_cfg_we(v_const_cfg_we),
    .const_cfg_addr(v_const_cfg_addr),
    .const_cfg_wdata(v_const_cfg_wdata),
    .scratch_cfg_we(v_scratch_cfg_we),
    .scratch_cfg_addr(v_scratch_cfg_addr),
    .scratch_cfg_wdata(v_scratch_cfg_wdata),
    .north_data_we(v_north_data_we),
    .north_data_out(v_north_data_out),
    .south_data_we(v_south_data_we),
    .south_data_out(v_south_data_out),
    .east_data_we(v_east_data_we),
    .east_data_out(v_east_data_out),
    .west_data_we(v_west_data_we),
    .west_data_out(v_west_data_out),
    .north_pred_we(v_north_pred_we),
    .north_pred_out(v_north_pred_out),
    .south_pred_we(v_south_pred_we),
    .south_pred_out(v_south_pred_out),
    .east_pred_we(v_east_pred_we),
    .east_pred_out(v_east_pred_out),
    .west_pred_we(v_west_pred_we),
    .west_pred_out(v_west_pred_out)
  );

  assign unused_outputs = (|h_north_data_we)
                          ^ (^h_north_data_out)
                          ^ (|h_south_data_we)
                          ^ (^h_south_data_out)
                          ^ (|h_east_data_we)
                          ^ (^h_east_data_out)
                          ^ (|h_west_data_we)
                          ^ (^h_west_data_out)
                          ^ (|h_north_pred_we)
                          ^ (^h_north_pred_out)
                          ^ (|h_south_pred_we)
                          ^ (^h_south_pred_out)
                          ^ (|h_east_pred_we)
                          ^ (^h_east_pred_out)
                          ^ (|h_west_pred_we)
                          ^ (^h_west_pred_out)
                          ^ (|v_north_data_we)
                          ^ (^v_north_data_out)
                          ^ (|v_south_data_we)
                          ^ (^v_south_data_out)
                          ^ (|v_east_data_we)
                          ^ (^v_east_data_out)
                          ^ (|v_west_data_we)
                          ^ (^v_west_data_out)
                          ^ (|v_north_pred_we)
                          ^ (^v_north_pred_out)
                          ^ (|v_south_pred_we)
                          ^ (^v_south_pred_out)
                          ^ (|v_east_pred_we)
                          ^ (^v_east_pred_out)
                          ^ (|v_west_pred_we)
                          ^ (^v_west_pred_out);

  always_comb begin
    if (unused_outputs) begin
    end
  end

/* verilator lint_off BLKSEQ */
  task automatic clear_ctrl;
    begin
      ctrl = '0;
      ctrl.exec_ctrl.op = OP_NOP;
      ctrl.lsu_ctrl.lsu_op = LSU_OP_NONE;
    end
  endtask

  task automatic set_h_ctrl(input int tile_idx);
    begin
      h_control_words[tile_idx*CONTROL_WORD_PHYSICAL_WIDTH +: CONTROL_WORD_PHYSICAL_WIDTH] <= pack_tile_control_word(ctrl);
    end
  endtask

  task automatic set_v_ctrl(input int tile_idx);
    begin
      v_control_words[tile_idx*CONTROL_WORD_PHYSICAL_WIDTH +: CONTROL_WORD_PHYSICAL_WIDTH] <= pack_tile_control_word(ctrl);
    end
  endtask

  task automatic set_all_nop;
    begin
      clear_ctrl();
      set_h_ctrl(0);
      set_h_ctrl(1);
      set_v_ctrl(0);
      set_v_ctrl(1);
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

  task automatic expect_pred(input string name,
                             input logic [PRED_WIDTH-1:0] actual,
                             input logic [PRED_WIDTH-1:0] expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected 0x%0h, got 0x%0h", name, expected, actual);
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

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    h_control_words = '0;
    h_const_cfg_we = '0;
    h_const_cfg_addr = '0;
    h_const_cfg_wdata = '0;
    h_scratch_cfg_we = '0;
    h_scratch_cfg_addr = '0;
    h_scratch_cfg_wdata = '0;
    v_control_words = '0;
    v_const_cfg_we = '0;
    v_const_cfg_addr = '0;
    v_const_cfg_wdata = '0;
    v_scratch_cfg_we = '0;
    v_scratch_cfg_addr = '0;
    v_scratch_cfg_wdata = '0;
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    h_const_cfg_we <= '0;
    h_scratch_cfg_we <= '0;
    h_scratch_cfg_addr <= '0;
    h_scratch_cfg_wdata <= '0;
    v_const_cfg_we <= '0;
    v_scratch_cfg_we <= '0;
    v_scratch_cfg_addr <= '0;
    v_scratch_cfg_wdata <= '0;

    unique case (cycle)
      0: begin
        rst_n <= 1'b0;
        set_all_nop();
      end
      1: begin
        h_const_cfg_we[0] <= 1'b1;
        h_const_cfg_addr[0*CONST_ADDR_WIDTH +: CONST_ADDR_WIDTH] <= 4'h0;
        h_const_cfg_wdata[0*DATA_WIDTH +: DATA_WIDTH] <= 32'haaaa_0001;
        v_const_cfg_we[0] <= 1'b1;
        v_const_cfg_addr[0*CONST_ADDR_WIDTH +: CONST_ADDR_WIDTH] <= 4'h0;
        v_const_cfg_wdata[0*DATA_WIDTH +: DATA_WIDTH] <= 32'hcccc_0003;
        set_all_nop();
      end
      2: begin
        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        set_h_ctrl(0);

        clear_ctrl();
        ctrl.pred_route_ctrl.west.we = 1'b1;
        ctrl.pred_route_ctrl.west.src = PRED_ROUTE_SRC_CONST_TRUE;
        set_h_ctrl(1);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.south.we = 1'b1;
        ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_CONST_DATA;
        set_v_ctrl(0);

        clear_ctrl();
        ctrl.pred_route_ctrl.north.we = 1'b1;
        ctrl.pred_route_ctrl.north.src = PRED_ROUTE_SRC_CONST_TRUE;
        set_v_ctrl(1);
      end
      3: begin
        expect_bit("horizontal source east data we", h_east_data_we[0], 1'b1);
        expect_data("horizontal source east data", h_east_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'haaaa_0001);
        expect_bit("horizontal source west pred we", h_west_pred_we[1], 1'b1);
        expect_pred("horizontal source west pred", h_west_pred_out[1*PRED_WIDTH +: PRED_WIDTH], 1'b1);
        expect_bit("vertical source south data we", v_south_data_we[0], 1'b1);
        expect_data("vertical source south data", v_south_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'hcccc_0003);
        expect_bit("vertical source north pred we", v_north_pred_we[1], 1'b1);
        expect_pred("vertical source north pred", v_north_pred_out[1*PRED_WIDTH +: PRED_WIDTH], 1'b1);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        ctrl.pred_route_ctrl.north.we = 1'b1;
        ctrl.pred_route_ctrl.north.src = PRED_ROUTE_SRC_EAST_PRED_IN;
        set_h_ctrl(0);

        clear_ctrl();
        ctrl.data_route_ctrl.north.we = 1'b1;
        ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        ctrl.pred_route_ctrl.west.we = 1'b1;
        ctrl.pred_route_ctrl.west.src = PRED_ROUTE_SRC_CONST_TRUE;
        set_h_ctrl(1);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.south.we = 1'b1;
        ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_CONST_DATA;
        ctrl.pred_route_ctrl.south.we = 1'b1;
        ctrl.pred_route_ctrl.south.src = PRED_ROUTE_SRC_SOUTH_PRED_IN;
        set_v_ctrl(0);

        clear_ctrl();
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_NORTH_DATA_IN;
        ctrl.pred_route_ctrl.north.we = 1'b1;
        ctrl.pred_route_ctrl.north.src = PRED_ROUTE_SRC_CONST_TRUE;
        set_v_ctrl(1);
      end
      4: begin
        expect_bit("horizontal received data we", h_north_data_we[1], 1'b1);
        expect_data("horizontal received data", h_north_data_out[1*DATA_WIDTH +: DATA_WIDTH], 32'haaaa_0001);
        expect_bit("horizontal received pred we", h_north_pred_we[0], 1'b1);
        expect_pred("horizontal received pred", h_north_pred_out[0*PRED_WIDTH +: PRED_WIDTH], 1'b1);
        expect_bit("vertical received data we", v_east_data_we[1], 1'b1);
        expect_data("vertical received data", v_east_data_out[1*DATA_WIDTH +: DATA_WIDTH], 32'hcccc_0003);
        expect_bit("vertical received pred we", v_south_pred_we[0], 1'b1);
        expect_pred("vertical received pred", v_south_pred_out[0*PRED_WIDTH +: PRED_WIDTH], 1'b1);
        $display("Mesh registered link test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Mesh link test timed out");
      end
    endcase
  end
/* verilator lint_on BLKSEQ */
endmodule : mesh_link_tb
