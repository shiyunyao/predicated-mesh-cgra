// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module tile_tb(input logic clk);
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
  int cycle;
  logic active_lsu_mode;
  logic w0_no_result_mode;
  logic pw0_no_result_mode;
  logic unused_tb_outputs;
  tile_control_word_t next_ctrl;

  tile dut (
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

  assign unused_tb_outputs = west_data_we
                             ^ (^west_data_out)
                             ^ south_pred_we
                             ^ south_pred_out[0]
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
    control_word = '0;
    const_cfg_we = 1'b0;
    const_cfg_addr = '0;
    const_cfg_wdata = '0;
    scratch_cfg_we = 1'b0;
    scratch_cfg_addr = '0;
    scratch_cfg_wdata = '0;
    north_data_in = 32'h0000_0003;
    north_data_valid = 1'b1;
    south_data_in = '0;
    south_data_valid = 1'b1;
    east_data_in = '0;
    east_data_valid = 1'b1;
    west_data_in = '0;
    west_data_valid = 1'b1;
    lsu_load_data = '0;
    lsu_load_valid = 1'b0;
    north_pred_in = 1'b1;
    north_pred_valid = 1'b1;
    south_pred_in = 1'b0;
    south_pred_valid = 1'b1;
    east_pred_in = 1'b1;
    east_pred_valid = 1'b1;
    west_pred_in = 1'b0;
    west_pred_valid = 1'b1;
    active_lsu_mode = ($test$plusargs("ACTIVE_LSU") != 0);
    w0_no_result_mode = ($test$plusargs("W0_NO_RESULT") != 0);
    pw0_no_result_mode = ($test$plusargs("PW0_NO_RESULT") != 0);
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
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
        const_cfg_wdata <= 32'h0000_0010;
        drive_nop();
      end
      1: begin
        const_cfg_we <= 1'b1;
        const_cfg_addr <= 4'h1;
        const_cfg_wdata <= 32'h0000_0020;
        if (active_lsu_mode) begin
          clear_next_ctrl();
          next_ctrl.lsu_ctrl.lsu_op = LSU_OP_LOAD;
          drive_next_ctrl();
        end else if (w0_no_result_mode) begin
          clear_next_ctrl();
          next_ctrl.exec_ctrl.op = OP_NOP;
          next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
          next_ctrl.data_rf_ctrl.data_w0_addr = 4'h0;
          drive_next_ctrl();
        end else if (pw0_no_result_mode) begin
          clear_next_ctrl();
          next_ctrl.exec_ctrl.op = OP_NOP;
          next_ctrl.pred_rf_ctrl.pred_w0_we = 1'b1;
          next_ctrl.pred_rf_ctrl.pred_w0_addr = 4'h0;
          drive_next_ctrl();
        end else begin
          drive_nop();
        end
      end
      2: begin
        if (active_lsu_mode) begin
          $fatal(1, "ACTIVE_LSU scenario did not trip tile assertion");
        end
        if (w0_no_result_mode) begin
          $fatal(1, "W0_NO_RESULT scenario did not trip tile assertion");
        end
        if (pw0_no_result_mode) begin
          $fatal(1, "PW0_NO_RESULT scenario did not trip tile assertion");
        end
        expect_bit("reset north data we", north_data_we, 1'b0);
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h0;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h0;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        drive_next_ctrl();
      end
      3: begin
        clear_next_ctrl();
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_NORTH_DATA_IN;
        drive_next_ctrl();
      end
      4: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_ADD;
        next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_RF_A;
        next_ctrl.exec_ctrl.src_b_sel = DATA_SRC_RF_B;
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h0;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h1;
        next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w0_addr = 4'h2;
        next_ctrl.data_route_ctrl.north.we = 1'b1;
        next_ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_FU_DATA;
        drive_next_ctrl();
      end
      5: begin
        expect_bit("FU route north we", north_data_we, 1'b1);
        expect_data("FU route north result", north_data_out, 32'h0000_0013);
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_PASS;
        next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_RF_A;
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h2;
        next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w0_addr = 4'h3;
        next_ctrl.data_route_ctrl.east.we = 1'b1;
        next_ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_RF_A;
        drive_next_ctrl();
      end
      6: begin
        expect_bit("RF route east we", east_data_we, 1'b1);
        expect_data("RF route east result", east_data_out, 32'h0000_0013);
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_CMP_EQ;
        next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_RF_A;
        next_ctrl.exec_ctrl.src_b_sel = DATA_SRC_RF_B;
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h2;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h3;
        next_ctrl.pred_rf_ctrl.pred_w0_we = 1'b1;
        next_ctrl.pred_rf_ctrl.pred_w0_addr = 4'h0;
        next_ctrl.pred_route_ctrl.north.we = 1'b1;
        next_ctrl.pred_route_ctrl.north.src = PRED_ROUTE_SRC_FU_PRED;
        drive_next_ctrl();
      end
      7: begin
        expect_bit("FU pred route north we", north_pred_we, 1'b1);
        expect_pred("FU pred route north result", north_pred_out, 1'b1);
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_PPASS;
        next_ctrl.exec_ctrl.src_p0_sel = PRED_SRC_RF_A;
        next_ctrl.exec_ctrl.pred_rf_raddr_a = 4'h0;
        next_ctrl.pred_rf_ctrl.pred_w0_we = 1'b1;
        next_ctrl.pred_rf_ctrl.pred_w0_addr = 4'h1;
        next_ctrl.pred_route_ctrl.east.we = 1'b1;
        next_ctrl.pred_route_ctrl.east.src = PRED_ROUTE_SRC_FU_PRED;
        drive_next_ctrl();
      end
      8: begin
        expect_bit("Predicate route east we", east_pred_we, 1'b1);
        expect_pred("Predicate route east result", east_pred_out, 1'b1);
        clear_next_ctrl();
        next_ctrl.const_ctrl.const_addr = 4'h1;
        next_ctrl.data_rf_ctrl.data_w1_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w1_addr = 4'h4;
        next_ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_CONST_DATA;
        next_ctrl.pred_rf_ctrl.pred_w1_we = 1'b1;
        next_ctrl.pred_rf_ctrl.pred_w1_addr = 4'h2;
        next_ctrl.pred_rf_ctrl.pred_w1_src = PRED_SRC_CONST_FALSE;
        drive_next_ctrl();
      end
      9: begin
        clear_next_ctrl();
        next_ctrl.exec_ctrl.op = OP_SELECT;
        next_ctrl.exec_ctrl.src_a_sel = DATA_SRC_RF_A;
        next_ctrl.exec_ctrl.src_b_sel = DATA_SRC_RF_B;
        next_ctrl.exec_ctrl.src_p0_sel = PRED_SRC_RF_B;
        next_ctrl.exec_ctrl.data_rf_raddr_a = 4'h4;
        next_ctrl.exec_ctrl.data_rf_raddr_b = 4'h1;
        next_ctrl.exec_ctrl.pred_rf_raddr_b = 4'h2;
        next_ctrl.data_rf_ctrl.data_w0_we = 1'b1;
        next_ctrl.data_rf_ctrl.data_w0_addr = 4'h5;
        next_ctrl.data_route_ctrl.south.we = 1'b1;
        next_ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_FU_DATA;
        drive_next_ctrl();
      end
      10: begin
        expect_bit("SELECT route south we", south_data_we, 1'b1);
        expect_data("SELECT false route south result", south_data_out, 32'h0000_0003);
        $display("Tile integration test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Tile integration test timed out");
      end
    endcase
  end
/* verilator lint_on BLKSEQ */
endmodule : tile_tb
