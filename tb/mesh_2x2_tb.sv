// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module mesh_2x2_tb(input logic clk);
  import cgra_pkg::*;

  localparam int ROWS = 2;
  localparam int COLS = 2;
  localparam int TILES = ROWS * COLS;
  localparam logic [TILES-1:0] LEFT_COLUMN_LSU_MASK = 4'b0101;

  logic rst_n;
  logic [TILES*CONTROL_WORD_PHYSICAL_WIDTH-1:0] control_words;
  logic [TILES-1:0] const_cfg_we;
  logic [TILES*CONST_ADDR_WIDTH-1:0] const_cfg_addr;
  logic [TILES*DATA_WIDTH-1:0] const_cfg_wdata;
  logic [TILES-1:0] scratch_cfg_we;
  logic [TILES*SCRATCH_ADDR_WIDTH-1:0] scratch_cfg_addr;
  logic [TILES*DATA_WIDTH-1:0] scratch_cfg_wdata;
  logic [TILES-1:0] north_data_we;
  logic [TILES*DATA_WIDTH-1:0] north_data_out;
  logic [TILES-1:0] south_data_we;
  logic [TILES*DATA_WIDTH-1:0] south_data_out;
  logic [TILES-1:0] east_data_we;
  logic [TILES*DATA_WIDTH-1:0] east_data_out;
  logic [TILES-1:0] west_data_we;
  logic [TILES*DATA_WIDTH-1:0] west_data_out;
  logic [TILES-1:0] north_pred_we;
  logic [TILES*PRED_WIDTH-1:0] north_pred_out;
  logic [TILES-1:0] south_pred_we;
  logic [TILES*PRED_WIDTH-1:0] south_pred_out;
  logic [TILES-1:0] east_pred_we;
  logic [TILES*PRED_WIDTH-1:0] east_pred_out;
  logic [TILES-1:0] west_pred_we;
  logic [TILES*PRED_WIDTH-1:0] west_pred_out;
  logic unused_outputs;
  logic non_lsu_mode;
  int cycle;
  tile_control_word_t ctrl;

  mesh #(
    .ROWS(ROWS),
    .COLS(COLS),
    .HAS_LSU_MASK(LEFT_COLUMN_LSU_MASK)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .control_words(control_words),
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
    .north_pred_we(north_pred_we),
    .north_pred_out(north_pred_out),
    .south_pred_we(south_pred_we),
    .south_pred_out(south_pred_out),
    .east_pred_we(east_pred_we),
    .east_pred_out(east_pred_out),
    .west_pred_we(west_pred_we),
    .west_pred_out(west_pred_out)
  );

  assign unused_outputs = (|north_data_we)
                          ^ (^north_data_out)
                          ^ (|south_data_we)
                          ^ (^south_data_out)
                          ^ (|east_data_we)
                          ^ (^east_data_out)
                          ^ (|west_data_we)
                          ^ (^west_data_out)
                          ^ (|north_pred_we)
                          ^ (^north_pred_out)
                          ^ (|south_pred_we)
                          ^ (^south_pred_out)
                          ^ (|east_pred_we)
                          ^ (^east_pred_out)
                          ^ (|west_pred_we)
                          ^ (^west_pred_out);

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

  task automatic set_ctrl(input int tile_idx);
    begin
      control_words[tile_idx*CONTROL_WORD_PHYSICAL_WIDTH +: CONTROL_WORD_PHYSICAL_WIDTH] <= pack_tile_control_word(ctrl);
    end
  endtask

  task automatic set_all_nop;
    begin
      clear_ctrl();
      set_ctrl(0);
      set_ctrl(1);
      set_ctrl(2);
      set_ctrl(3);
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

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    control_words = '0;
    const_cfg_we = '0;
    const_cfg_addr = '0;
    const_cfg_wdata = '0;
    scratch_cfg_we = '0;
    scratch_cfg_addr = '0;
    scratch_cfg_wdata = '0;
    non_lsu_mode = ($test$plusargs("NON_LSU_RIGHT") != 0);
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    const_cfg_we <= '0;
    scratch_cfg_we <= '0;
    scratch_cfg_addr <= '0;
    scratch_cfg_wdata <= '0;

    unique case (cycle)
      0: begin
        rst_n <= 1'b0;
        set_all_nop();
      end
      1: begin
        if (non_lsu_mode) begin
          clear_ctrl();
          ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
          ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_ZERO;
          set_ctrl(1);
        end else begin
          const_cfg_we[0] <= 1'b1;
          const_cfg_addr[0*CONST_ADDR_WIDTH +: CONST_ADDR_WIDTH] <= 4'h0;
          const_cfg_wdata[0*DATA_WIDTH +: DATA_WIDTH] <= 32'haaaa_0001;
          const_cfg_we[2] <= 1'b1;
          const_cfg_addr[2*CONST_ADDR_WIDTH +: CONST_ADDR_WIDTH] <= 4'h0;
          const_cfg_wdata[2*DATA_WIDTH +: DATA_WIDTH] <= 32'hbbbb_0002;

          clear_ctrl();
          ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
          ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_ZERO;
          set_ctrl(0);
          set_ctrl(2);
        end
      end
      2: begin
        if (non_lsu_mode) begin
          $fatal(1, "NON_LSU_RIGHT scenario did not trip mesh LSU mask assertion");
        end
        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        set_ctrl(0);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        set_ctrl(2);

        clear_ctrl();
        set_ctrl(1);
        set_ctrl(3);
      end
      3: begin
        expect_bit("top-left east route valid", east_data_we[0], 1'b1);
        expect_data("top-left east route data", east_data_out[0*DATA_WIDTH +: DATA_WIDTH], 32'haaaa_0001);
        expect_bit("bottom-left east route valid", east_data_we[2], 1'b1);
        expect_data("bottom-left east route data", east_data_out[2*DATA_WIDTH +: DATA_WIDTH], 32'hbbbb_0002);

        clear_ctrl();
        ctrl.data_route_ctrl.north.we = 1'b1;
        ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        ctrl.data_route_ctrl.south.we = 1'b1;
        ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        set_ctrl(1);

        clear_ctrl();
        ctrl.data_route_ctrl.north.we = 1'b1;
        ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        set_ctrl(3);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        set_ctrl(0);
        set_ctrl(2);
      end
      4: begin
        expect_bit("one-hop top-right north valid", north_data_we[1], 1'b1);
        expect_data("one-hop top-right data", north_data_out[1*DATA_WIDTH +: DATA_WIDTH], 32'haaaa_0001);
        expect_bit("one-hop bottom-right north valid", north_data_we[3], 1'b1);
        expect_data("one-hop bottom-right data", north_data_out[3*DATA_WIDTH +: DATA_WIDTH], 32'hbbbb_0002);

        clear_ctrl();
        ctrl.data_route_ctrl.south.we = 1'b1;
        ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_WEST_DATA_IN;
        set_ctrl(1);

        clear_ctrl();
        ctrl.data_route_ctrl.north.we = 1'b1;
        ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_NORTH_DATA_IN;
        set_ctrl(3);

        clear_ctrl();
        ctrl.const_ctrl.const_addr = 4'h0;
        ctrl.data_route_ctrl.east.we = 1'b1;
        ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_CONST_DATA;
        set_ctrl(0);
        set_ctrl(2);
      end
      5: begin
        expect_bit("two-hop bottom-right north valid", north_data_we[3], 1'b1);
        expect_data("two-hop bottom-right data", north_data_out[3*DATA_WIDTH +: DATA_WIDTH], 32'haaaa_0001);
        $display("2x2 mesh integration test passed");
        $finish;
      end
      default: begin
        $fatal(1, "2x2 mesh integration test timed out");
      end
    endcase
  end
/* verilator lint_on BLKSEQ */
endmodule : mesh_2x2_tb
