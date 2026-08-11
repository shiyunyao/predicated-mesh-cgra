// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_src_mux_tb(input logic clk);
  import cgra_pkg::*;

  logic consume;
  data_src_e src_sel;
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
  logic [DATA_WIDTH-1:0] lsu_load_data;
  logic lsu_load_valid;
  logic [DATA_WIDTH-1:0] data_out;
  logic src_valid;
  int cycle;
  logic invalid_network_mode;
  logic invalid_lsu_mode;

  data_src_mux dut (
    .consume(consume),
    .src_sel(src_sel),
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
    .lsu_load_data(lsu_load_data),
    .lsu_load_valid(lsu_load_valid),
    .data_out(data_out),
    .src_valid(src_valid)
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
    consume = 1'b0;
    src_sel = DATA_SRC_RF_A;
    rf_a_data = 32'h1111_aaaa;
    rf_b_data = 32'h2222_bbbb;
    north_data_in = 32'h3333_cccc;
    north_data_valid = 1'b1;
    south_data_in = 32'h4444_dddd;
    south_data_valid = 1'b1;
    east_data_in = 32'h5555_eeee;
    east_data_valid = 1'b1;
    west_data_in = 32'h6666_ffff;
    west_data_valid = 1'b1;
    const_data = 32'h7777_1234;
    lsu_load_data = 32'h8888_5678;
    lsu_load_valid = 1'b1;
    invalid_network_mode = ($test$plusargs("INVALID_NETWORK") != 0);
    invalid_lsu_mode = ($test$plusargs("INVALID_LSU") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    consume <= 1'b1;

    unique case (cycle)
      0: begin
        if (invalid_network_mode) begin
          src_sel <= DATA_SRC_NORTH_DATA_IN;
          north_data_valid <= 1'b0;
        end else if (invalid_lsu_mode) begin
          src_sel <= DATA_SRC_LSU_LOAD_DATA;
          lsu_load_valid <= 1'b0;
        end else begin
          src_sel <= DATA_SRC_RF_A;
        end
      end
      1: begin
        if (invalid_network_mode) begin
          $fatal(1, "INVALID_NETWORK scenario did not trip data source assertion");
        end
        if (invalid_lsu_mode) begin
          $fatal(1, "INVALID_LSU scenario did not trip data source assertion");
        end
        expect_data("RF_A", data_out, 32'h1111_aaaa);
        expect_bit("RF_A valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_RF_B;
      end
      2: begin
        expect_data("RF_B", data_out, 32'h2222_bbbb);
        expect_bit("RF_B valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_NORTH_DATA_IN;
      end
      3: begin
        expect_data("NORTH_DATA_IN", data_out, 32'h3333_cccc);
        expect_bit("NORTH_DATA_IN valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_SOUTH_DATA_IN;
      end
      4: begin
        expect_data("SOUTH_DATA_IN", data_out, 32'h4444_dddd);
        expect_bit("SOUTH_DATA_IN valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_EAST_DATA_IN;
      end
      5: begin
        expect_data("EAST_DATA_IN", data_out, 32'h5555_eeee);
        expect_bit("EAST_DATA_IN valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_WEST_DATA_IN;
      end
      6: begin
        expect_data("WEST_DATA_IN", data_out, 32'h6666_ffff);
        expect_bit("WEST_DATA_IN valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_CONST_DATA;
      end
      7: begin
        expect_data("CONST_DATA", data_out, 32'h7777_1234);
        expect_bit("CONST_DATA valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_LSU_LOAD_DATA;
      end
      8: begin
        expect_data("LSU_LOAD_DATA", data_out, 32'h8888_5678);
        expect_bit("LSU_LOAD_DATA valid", src_valid, 1'b1);
        src_sel <= DATA_SRC_ZERO;
        north_data_valid <= 1'b0;
        lsu_load_valid <= 1'b0;
      end
      9: begin
        expect_data("ZERO", data_out, 32'h0000_0000);
        expect_bit("ZERO valid", src_valid, 1'b1);
        $display("Data source mux test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Data source mux test timed out");
      end
    endcase
  end
endmodule : data_src_mux_tb
