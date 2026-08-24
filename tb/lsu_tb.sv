// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module lsu_tb(input logic clk);
  import cgra_pkg::*;

  logic rst_n;
  logic has_lsu;
  lsu_op_e lsu_op;
  data_src_e lsu_addr_src;
  data_src_e lsu_store_data_src;
  logic lsu_commit_pred_enable;
  logic lsu_commit_pred_invert;
  pred_src_e lsu_commit_pred_src;
  logic [DATA_WIDTH-1:0] rf_a_data;
  logic [DATA_WIDTH-1:0] rf_b_data;
  logic [DATA_WIDTH-1:0] north_data_in;
  logic north_data_valid;
  logic [DATA_WIDTH-1:0] south_data_in;
  logic south_data_valid;
  logic [DATA_WIDTH-1:0] east_data_in;
  logic east_data_valid;
  logic [DATA_WIDTH-1:0] west_data_in;
  logic west_data_valid;
  logic [DATA_WIDTH-1:0] const_data;
  logic [DATA_WIDTH-1:0] lsu_load_data_src;
  logic lsu_load_data_valid;
  logic [PRED_WIDTH-1:0] rf_a_pred;
  logic [PRED_WIDTH-1:0] rf_b_pred;
  logic [PRED_WIDTH-1:0] north_pred_in;
  logic north_pred_valid;
  logic [PRED_WIDTH-1:0] south_pred_in;
  logic south_pred_valid;
  logic [PRED_WIDTH-1:0] east_pred_in;
  logic east_pred_valid;
  logic [PRED_WIDTH-1:0] west_pred_in;
  logic west_pred_valid;
  logic load_resp_valid;
  logic [DATA_WIDTH-1:0] load_resp_data;
  logic mem_req_valid;
  logic mem_req_write;
  logic [SCRATCHPAD_ADDR_WIDTH-1:0] mem_req_addr;
  logic [DATA_WIDTH-1:0] mem_req_wdata;
  logic [DATA_WIDTH-1:0] mem_read_data;
  int cycle;
  logic load_pred_mode;
  logic non_lsu_mode;
  logic invalid_data_mode;
  logic invalid_pred_mode;
  logic out_of_range_mode;

  lsu dut (
    .clk(clk),
    .rst_n(rst_n),
    .has_lsu(has_lsu),
    .lsu_op(lsu_op),
    .lsu_addr_src(lsu_addr_src),
    .lsu_store_data_src(lsu_store_data_src),
    .lsu_commit_pred_enable(lsu_commit_pred_enable),
    .lsu_commit_pred_invert(lsu_commit_pred_invert),
    .lsu_commit_pred_src(lsu_commit_pred_src),
    .rf_a_data(rf_a_data),
    .rf_b_data(rf_b_data),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data_src(lsu_load_data_src),
    .lsu_load_data_valid(lsu_load_data_valid),
    .rf_a_pred(rf_a_pred),
    .rf_b_pred(rf_b_pred),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .load_resp_valid(load_resp_valid),
    .load_resp_data(load_resp_data),
    .mem_req_valid(mem_req_valid),
    .mem_req_write(mem_req_write),
    .mem_req_addr(mem_req_addr),
    .mem_req_wdata(mem_req_wdata),
    .mem_read_data(mem_read_data)
  );

  shared_scratchpad #(
    .PORTS(1)
  ) memory_i (
    .clk(clk),
    .req_valid(mem_req_valid),
    .req_write(mem_req_write),
    .req_addr(mem_req_addr),
    .req_wdata(mem_req_wdata),
    .read_data(mem_read_data),
    .cfg_write_en(1'b0),
    .cfg_write_addr('0),
    .cfg_write_data('0)
  );

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
    has_lsu = 1'b1;
    lsu_op = LSU_OP_NONE;
    lsu_addr_src = DATA_SRC_RF_A;
    lsu_store_data_src = DATA_SRC_RF_B;
    lsu_commit_pred_enable = 1'b0;
    lsu_commit_pred_invert = 1'b0;
    lsu_commit_pred_src = PRED_SRC_RF_A;
    rf_a_data = '0;
    rf_b_data = '0;
    north_data_in = '0;
    north_data_valid = 1'b1;
    south_data_in = '0;
    south_data_valid = 1'b1;
    east_data_in = '0;
    east_data_valid = 1'b1;
    west_data_in = '0;
    west_data_valid = 1'b1;
    const_data = '0;
    lsu_load_data_src = '0;
    lsu_load_data_valid = 1'b1;
    rf_a_pred = 1'b1;
    rf_b_pred = 1'b0;
    north_pred_in = 1'b1;
    north_pred_valid = 1'b1;
    south_pred_in = 1'b0;
    south_pred_valid = 1'b1;
    east_pred_in = 1'b1;
    east_pred_valid = 1'b1;
    west_pred_in = 1'b0;
    west_pred_valid = 1'b1;
    load_pred_mode = ($test$plusargs("LOAD_PRED") != 0);
    non_lsu_mode = ($test$plusargs("NON_LSU") != 0);
    invalid_data_mode = ($test$plusargs("INVALID_DATA") != 0);
    invalid_pred_mode = ($test$plusargs("INVALID_PRED") != 0);
    out_of_range_mode = ($test$plusargs("OUT_OF_RANGE") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    rst_n <= 1'b1;
    has_lsu <= 1'b1;
    lsu_op <= LSU_OP_NONE;
    lsu_addr_src <= DATA_SRC_RF_A;
    lsu_store_data_src <= DATA_SRC_RF_B;
    lsu_commit_pred_enable <= 1'b0;
    lsu_commit_pred_invert <= 1'b0;
    lsu_commit_pred_src <= PRED_SRC_RF_A;
    north_data_valid <= 1'b1;
    north_pred_valid <= 1'b1;

    unique case (cycle)
      0: begin
        rst_n <= 1'b0;
      end
      1: begin
        if (load_pred_mode) begin
          lsu_op <= LSU_OP_LOAD;
          lsu_commit_pred_enable <= 1'b1;
        end else if (non_lsu_mode) begin
          has_lsu <= 1'b0;
          lsu_op <= LSU_OP_LOAD;
        end else if (invalid_data_mode) begin
          lsu_op <= LSU_OP_STORE;
          lsu_addr_src <= DATA_SRC_NORTH_DATA_IN;
          north_data_valid <= 1'b0;
        end else if (invalid_pred_mode) begin
          lsu_op <= LSU_OP_STORE;
          lsu_commit_pred_enable <= 1'b1;
          lsu_commit_pred_src <= PRED_SRC_NORTH_PRED_IN;
          north_pred_valid <= 1'b0;
        end else if (out_of_range_mode) begin
          lsu_op <= LSU_OP_LOAD;
          rf_a_data <= SCRATCHPAD_DEPTH;
        end
      end
      2: begin
        if (load_pred_mode) begin
          $fatal(1, "LOAD_PRED scenario did not trip LSU assertion");
        end
        if (non_lsu_mode) begin
          $fatal(1, "NON_LSU scenario did not trip LSU assertion");
        end
        if (invalid_data_mode) begin
          $fatal(1, "INVALID_DATA scenario did not trip LSU assertion");
        end
        if (invalid_pred_mode) begin
          $fatal(1, "INVALID_PRED scenario did not trip LSU assertion");
        end
        if (out_of_range_mode) begin
          $fatal(1, "OUT_OF_RANGE scenario did not trip LSU assertion");
        end
        expect_bit("reset load_resp_valid", load_resp_valid, 1'b0);
        lsu_op <= LSU_OP_STORE;
        rf_a_data <= 32'd5;
        rf_b_data <= 32'haaaa_0001;
      end
      3: begin
        expect_bit("no hidden store response", load_resp_valid, 1'b0);
        expect_bit("store request valid", mem_req_valid, 1'b1);
        expect_bit("store request write", mem_req_write, 1'b1);
        expect_data("store request address", DATA_WIDTH'(mem_req_addr), 32'd5);
        expect_data("store request data", mem_req_wdata, 32'haaaa_0001);
        lsu_op <= LSU_OP_LOAD;
        rf_a_data <= 32'd5;
      end
      4: begin
        expect_bit("load request valid", mem_req_valid, 1'b1);
        expect_bit("load request read", mem_req_write, 1'b0);
        expect_data("load request address", DATA_WIDTH'(mem_req_addr), 32'd5);
        expect_bit("load latency cycle 1", load_resp_valid, 1'b0);
      end
      5: begin
        expect_bit("load latency cycle 2 valid", load_resp_valid, 1'b1);
        expect_data("loaded unconditional store", load_resp_data, 32'haaaa_0001);
        lsu_op <= LSU_OP_STORE;
        rf_a_data <= 32'd6;
        rf_b_data <= 32'hbbbb_0002;
        lsu_commit_pred_enable <= 1'b1;
        lsu_commit_pred_src <= PRED_SRC_RF_A;
        rf_a_pred <= 1'b1;
      end
      6: begin
        expect_bit("load response one-cycle pulse", load_resp_valid, 1'b0);
        expect_bit("true predicated store request", mem_req_valid, 1'b1);
        expect_bit("true predicated store write", mem_req_write, 1'b1);
        lsu_op <= LSU_OP_STORE;
        rf_a_data <= 32'd7;
        rf_b_data <= 32'hcccc_0003;
        lsu_commit_pred_enable <= 1'b1;
        lsu_commit_pred_src <= PRED_SRC_RF_A;
        rf_a_pred <= 1'b0;
      end
      7: begin
        expect_bit("false predicated store suppressed", mem_req_valid, 1'b0);
        lsu_op <= LSU_OP_STORE;
        rf_a_data <= 32'd8;
        rf_b_data <= 32'hdddd_0004;
        lsu_commit_pred_enable <= 1'b1;
        lsu_commit_pred_invert <= 1'b1;
        lsu_commit_pred_src <= PRED_SRC_RF_A;
        rf_a_pred <= 1'b0;
      end
      8: begin
        expect_bit("inverted predicated store request", mem_req_valid, 1'b1);
        expect_bit("inverted predicated store write", mem_req_write, 1'b1);
        lsu_op <= LSU_OP_LOAD;
        rf_a_data <= 32'd6;
      end
      9: begin
      end
      10: begin
        expect_bit("predicate true load valid", load_resp_valid, 1'b1);
        expect_data("predicate true store", load_resp_data, 32'hbbbb_0002);
        lsu_op <= LSU_OP_LOAD;
        rf_a_data <= 32'd8;
      end
      11: begin
        expect_bit("between load responses", load_resp_valid, 1'b0);
      end
      12: begin
        expect_bit("inverted predicate load valid", load_resp_valid, 1'b1);
        expect_data("inverted predicate store", load_resp_data, 32'hdddd_0004);
        $display("LSU test passed");
        $finish;
      end
      default: begin
        $fatal(1, "LSU test timed out");
      end
    endcase
  end
endmodule : lsu_tb
