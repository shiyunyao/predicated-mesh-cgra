// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module tile_lsu_tb(input logic clk);
  import cgra_pkg::*;

  logic rst_n;
  control_word_bits_t control_word;
  logic const_cfg_we;
  logic [CONST_ADDR_WIDTH-1:0] const_cfg_addr;
  logic [DATA_WIDTH-1:0] const_cfg_wdata;
  logic scratch_cfg_we;
  logic [SCRATCH_ADDR_WIDTH-1:0] scratch_cfg_addr;
  logic [DATA_WIDTH-1:0] scratch_cfg_wdata;
  logic [DATA_WIDTH-1:0] north_data_in;
  logic north_data_valid;
  logic [DATA_WIDTH-1:0] south_data_in;
  logic south_data_valid;
  logic [DATA_WIDTH-1:0] east_data_in;
  logic east_data_valid;
  logic [DATA_WIDTH-1:0] west_data_in;
  logic west_data_valid;
  logic [DATA_WIDTH-1:0] lsu_load_data;
  logic lsu_load_valid;
  logic [PRED_WIDTH-1:0] north_pred_in;
  logic north_pred_valid;
  logic [PRED_WIDTH-1:0] south_pred_in;
  logic south_pred_valid;
  logic [PRED_WIDTH-1:0] east_pred_in;
  logic east_pred_valid;
  logic [PRED_WIDTH-1:0] west_pred_in;
  logic west_pred_valid;
  logic north_data_we;
  logic [DATA_WIDTH-1:0] north_data_out;
  logic south_data_we;
  logic [DATA_WIDTH-1:0] south_data_out;
  logic east_data_we;
  logic [DATA_WIDTH-1:0] east_data_out;
  logic west_data_we;
  logic [DATA_WIDTH-1:0] west_data_out;
  logic north_pred_we;
  logic [PRED_WIDTH-1:0] north_pred_out;
  logic south_pred_we;
  logic [PRED_WIDTH-1:0] south_pred_out;
  logic east_pred_we;
  logic [PRED_WIDTH-1:0] east_pred_out;
  logic west_pred_we;
  logic [PRED_WIDTH-1:0] west_pred_out;
  logic unused_tb_outputs;
  int cycle;
  tile_control_word_t next_ctrl;

  tile #(
    .HAS_LSU(1'b1)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .control_word(control_word),
    .const_cfg_we(const_cfg_we),
    .const_cfg_addr(const_cfg_addr),
    .const_cfg_wdata(const_cfg_wdata),
    .scratch_cfg_we(scratch_cfg_we),
    .scratch_cfg_addr(scratch_cfg_addr),
    .scratch_cfg_wdata(scratch_cfg_wdata),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .lsu_load_data(lsu_load_data),
    .lsu_load_valid(lsu_load_valid),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
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

  assign unused_tb_outputs = north_pred_we
                             ^ north_pred_out[0]
                             ^ south_pred_we
                             ^ south_pred_out[0]
                             ^ east_pred_we
                             ^ east_pred_out[0]
                             ^ west_pred_we
                             ^ west_pred_out[0];

  always_comb begin
    if (unused_tb_outputs) begin
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

  task automatic drive_next_ctrl;
    begin
      control_word <= pack_tile_control_word(next_ctrl);
    end
  endtask

  task automatic drive_nop;
    begin
      clear_next_ctrl();
      drive_next_ctrl();
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
    control_word = '0;
    const_cfg_we = 1'b0;
    const_cfg_addr = '0;
    const_cfg_wdata = '0;
    scratch_cfg_we = 1'b0;
    scratch_cfg_addr = '0;
    scratch_cfg_wdata = '0;
    north_data_in = '0;
    north_data_valid = 1'b1;
    south_data_in = '0;
    south_data_valid = 1'b1;
    east_data_in = '0;
    east_data_valid = 1'b1;
    west_data_in = '0;
    west_data_valid = 1'b1;
    lsu_load_data = 32'hdead_beef;
    lsu_load_valid = 1'b1;
    north_pred_in = 1'b0;
    north_pred_valid = 1'b1;
    south_pred_in = 1'b0;
    south_pred_valid = 1'b1;
    east_pred_in = 1'b0;
    east_pred_valid = 1'b1;
    west_pred_in = 1'b0;
    west_pred_valid = 1'b1;
  end

/* verilator lint_off BLKSEQ */
  always_ff @(posedge clk or negedge clk) begin
    if (clk) begin
      unique case (cycle)
        12, 17, 20, 25: begin
          control_word <= '0;
        end
        default: begin
        end
      endcase
    end else begin
      cycle <= cycle + 1;
      rst_n <= 1'b1;
      const_cfg_we <= 1'b0;
      scratch_cfg_we <= 1'b0;
      scratch_cfg_addr <= '0;
      scratch_cfg_wdata <= '0;

      unique case (cycle)
      0: begin
        rst_n <= 1'b0;
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h0;
        const_cfg_wdata <= 32'd5;
        drive_nop();
      end
      1: begin
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h1;
        const_cfg_wdata <= 32'h1111_0001;
        drive_nop();
      end
      2: begin
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h2;
        const_cfg_wdata <= 32'h2222_0002;
        drive_nop();
      end
      3: begin
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h3;
        const_cfg_wdata <= 32'd9;
        drive_nop();
      end
      4: begin
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h4;
        const_cfg_wdata <= 32'h3333_0003;
        drive_nop();
      end
      5: begin
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h0;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h0;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        drive_next_ctrl();
      end
      6: begin
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        drive_next_ctrl();
      end
      7: begin
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h3;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h4;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        next_ctrl.pred_rf_ctrl.pred_w1_we = 1'b1;
        next_ctrl.pred_rf_ctrl.pred_w1_addr = 4'h0;
        next_ctrl.pred_rf_ctrl.pred_w1_src = PRED_SRC_CONST_TRUE;
        drive_next_ctrl();
      end
      8: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h1;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        next_ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_RF_B;
        drive_next_ctrl();
      end
      9: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        drive_next_ctrl();
      end
      10: begin
        drive_nop();
      end
      11: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_PASS;
        next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_LSU_LOAD_DATA;
        next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w0_addr = 4'h3;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h2;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_LSU_LOAD_DATA;
        next_ctrl.data_route_ctrl.north.we = 1'b1;
        next_ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_LSU_LOAD_DATA;
        drive_next_ctrl();
      end
      12: begin
        expect_bit("load route north we", north_data_we, 1'b1);
        expect_data("load route north data", north_data_out, 32'h1111_0001);
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h2;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        next_ctrl.pred_rf_ctrl.pred_w1_we = 1'b1;
        next_ctrl.pred_rf_ctrl.pred_w1_addr = 4'h1;
        next_ctrl.pred_rf_ctrl.pred_w1_src = PRED_SRC_CONST_FALSE;
        drive_next_ctrl();
      end
      13: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h1;
        next_ctrl.exec_ctrl.pred_rf_raddr_a = 4'h0;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        next_ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_RF_B;
        next_ctrl.lsu_ctrl.lsu_commit_pred_enable = 1'b1;
        next_ctrl.lsu_ctrl.lsu_commit_pred_src = PRED_SRC_RF_A;
        drive_next_ctrl();
      end
      14: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        drive_next_ctrl();
      end
      15: begin
        drive_nop();
      end
      16: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h4;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        next_ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_LSU_LOAD_DATA;
        next_ctrl.data_route_ctrl.east.we = 1'b1;
        next_ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_LSU_LOAD_DATA;
        drive_next_ctrl();
      end
      17: begin
        expect_bit("predicate store route east we", east_data_we, 1'b1);
        expect_data("predicate store route east data", east_data_out, 32'h2222_0002);
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h4;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        drive_next_ctrl();
      end
      18: begin
        drive_nop();
      end
      19: begin
        clear_next_ctrl();
        next_ctrl.data_route_ctrl.south.we = 1'b1;
        next_ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_LSU_LOAD_DATA;
        drive_next_ctrl();
      end
      20: begin
        expect_bit("load-as-store-data route south we", south_data_we, 1'b1);
        expect_data("load-as-store-data route south data", south_data_out, 32'h2222_0002);
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h4;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        drive_next_ctrl();
      end
      21: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h1;
        next_ctrl.exec_ctrl.pred_rf_raddr_b = 4'h1;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        next_ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_RF_B;
        next_ctrl.lsu_ctrl.lsu_commit_pred_enable = 1'b1;
        next_ctrl.lsu_ctrl.lsu_commit_pred_src = PRED_SRC_RF_B;
        drive_next_ctrl();
      end
      22: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
        next_ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
        drive_next_ctrl();
      end
      23: begin
        drive_nop();
      end
      24: begin
        clear_next_ctrl();
        next_ctrl.data_route_ctrl.west.we = 1'b1;
        next_ctrl.data_route_ctrl.west.src = DATA_ROUTE_SRC_LSU_LOAD_DATA;
        drive_next_ctrl();
      end
      25: begin
        expect_bit("predicate false route west we", west_data_we, 1'b1);
        expect_data("predicate false preserved data", west_data_out, 32'h2222_0002);
        $display("Tile LSU integration test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Tile LSU integration test timed out");
      end
    endcase
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : tile_lsu_tb
