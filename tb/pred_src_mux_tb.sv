// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_src_mux_tb(input logic clk);
  import cgra_pkg::*;

  logic consume;
  pred_src_e src_sel;
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
  logic [PRED_WIDTH-1:0] pred_out;
  logic src_valid;
  int cycle;
  logic invalid_network_mode;

  pred_src_mux dut (
    .consume(consume),
    .src_sel(src_sel),
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
    .pred_out(pred_out),
    .src_valid(src_valid)
  );

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
    consume = 1'b0;
    src_sel = PRED_SRC_RF_A;
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
    invalid_network_mode = ($test$plusargs("INVALID_NETWORK") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    consume <= 1'b1;

    unique case (cycle)
      0: begin
        if (invalid_network_mode) begin
          src_sel <= PRED_SRC_NORTH_PRED_IN;
          north_pred_valid <= 1'b0;
        end else begin
          src_sel <= PRED_SRC_RF_A;
        end
      end
      1: begin
        if (invalid_network_mode) begin
          $fatal(1, "INVALID_NETWORK scenario did not trip predicate source assertion");
        end
        expect_pred("RF_A", pred_out, 1'b1);
        expect_bit("RF_A valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_RF_B;
      end
      2: begin
        expect_pred("RF_B", pred_out, 1'b0);
        expect_bit("RF_B valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_NORTH_PRED_IN;
      end
      3: begin
        expect_pred("NORTH_PRED_IN", pred_out, 1'b1);
        expect_bit("NORTH_PRED_IN valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_SOUTH_PRED_IN;
      end
      4: begin
        expect_pred("SOUTH_PRED_IN", pred_out, 1'b0);
        expect_bit("SOUTH_PRED_IN valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_EAST_PRED_IN;
      end
      5: begin
        expect_pred("EAST_PRED_IN", pred_out, 1'b1);
        expect_bit("EAST_PRED_IN valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_WEST_PRED_IN;
      end
      6: begin
        expect_pred("WEST_PRED_IN", pred_out, 1'b0);
        expect_bit("WEST_PRED_IN valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_CONST_TRUE;
        north_pred_valid <= 1'b0;
      end
      7: begin
        expect_pred("CONST_TRUE", pred_out, 1'b1);
        expect_bit("CONST_TRUE valid", src_valid, 1'b1);
        src_sel <= PRED_SRC_CONST_FALSE;
      end
      8: begin
        expect_pred("CONST_FALSE", pred_out, 1'b0);
        expect_bit("CONST_FALSE valid", src_valid, 1'b1);
        $display("Predicate source mux test passed");
        $finish;
      end
      default: begin
        $fatal(1, "Predicate source mux test timed out");
      end
    endcase
  end
endmodule : pred_src_mux_tb
