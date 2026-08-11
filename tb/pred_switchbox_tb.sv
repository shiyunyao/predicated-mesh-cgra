// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_switchbox_tb(input logic clk);
  import cgra_pkg::*;

  logic north_we;
  pred_route_src_e north_src;
  logic south_we;
  pred_route_src_e south_src;
  logic east_we;
  pred_route_src_e east_src;
  logic west_we;
  pred_route_src_e west_src;
  logic [PRED_WIDTH-1:0] north_pred_in;
  logic north_pred_valid;
  logic [PRED_WIDTH-1:0] south_pred_in;
  logic south_pred_valid;
  logic [PRED_WIDTH-1:0] east_pred_in;
  logic east_pred_valid;
  logic [PRED_WIDTH-1:0] west_pred_in;
  logic west_pred_valid;
  logic [PRED_WIDTH-1:0] fu_pred;
  logic fu_pred_valid;
  logic [PRED_WIDTH-1:0] rf_a_pred;
  logic [PRED_WIDTH-1:0] rf_b_pred;
  logic north_pred_we;
  logic [PRED_WIDTH-1:0] north_pred_out;
  logic south_pred_we;
  logic [PRED_WIDTH-1:0] south_pred_out;
  logic east_pred_we;
  logic [PRED_WIDTH-1:0] east_pred_out;
  logic west_pred_we;
  logic [PRED_WIDTH-1:0] west_pred_out;
  int cycle;
  logic invalid_none_mode;
  logic invalid_network_mode;
  logic invalid_fu_mode;

  pred_switchbox dut (
    .north_we(north_we),
    .north_src(north_src),
    .south_we(south_we),
    .south_src(south_src),
    .east_we(east_we),
    .east_src(east_src),
    .west_we(west_we),
    .west_src(west_src),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .fu_pred(fu_pred),
    .fu_pred_valid(fu_pred_valid),
    .rf_a_pred(rf_a_pred),
    .rf_b_pred(rf_b_pred),
    .north_pred_we(north_pred_we),
    .north_pred_out(north_pred_out),
    .south_pred_we(south_pred_we),
    .south_pred_out(south_pred_out),
    .east_pred_we(east_pred_we),
    .east_pred_out(east_pred_out),
    .west_pred_we(west_pred_we),
    .west_pred_out(west_pred_out)
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
    north_we = 1'b0;
    north_src = PRED_ROUTE_SRC_NONE;
    south_we = 1'b0;
    south_src = PRED_ROUTE_SRC_NONE;
    east_we = 1'b0;
    east_src = PRED_ROUTE_SRC_NONE;
    west_we = 1'b0;
    west_src = PRED_ROUTE_SRC_NONE;
    north_pred_in = 1'b1;
    north_pred_valid = 1'b1;
    south_pred_in = 1'b0;
    south_pred_valid = 1'b1;
    east_pred_in = 1'b1;
    east_pred_valid = 1'b1;
    west_pred_in = 1'b0;
    west_pred_valid = 1'b1;
    fu_pred = 1'b1;
    fu_pred_valid = 1'b1;
    rf_a_pred = 1'b0;
    rf_b_pred = 1'b1;
    invalid_none_mode = ($test$plusargs("INVALID_NONE") != 0);
    invalid_network_mode = ($test$plusargs("INVALID_NETWORK") != 0);
    invalid_fu_mode = ($test$plusargs("INVALID_FU") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    unique case (cycle)
      0: begin
        if (invalid_none_mode) begin
          north_we <= 1'b1;
          north_src <= PRED_ROUTE_SRC_NONE;
        end else if (invalid_network_mode) begin
          south_we <= 1'b1;
          south_src <= PRED_ROUTE_SRC_NORTH_PRED_IN;
          north_pred_valid <= 1'b0;
        end else if (invalid_fu_mode) begin
          east_we <= 1'b1;
          east_src <= PRED_ROUTE_SRC_FU_PRED;
          fu_pred_valid <= 1'b0;
        end else begin
          north_we <= 1'b1;
          north_src <= PRED_ROUTE_SRC_NORTH_PRED_IN;
          south_we <= 1'b1;
          south_src <= PRED_ROUTE_SRC_SOUTH_PRED_IN;
          east_we <= 1'b1;
          east_src <= PRED_ROUTE_SRC_EAST_PRED_IN;
          west_we <= 1'b1;
          west_src <= PRED_ROUTE_SRC_WEST_PRED_IN;
        end
      end
      1: begin
        if (invalid_none_mode) begin
          $fatal(1, "INVALID_NONE scenario did not trip PredicateSwitchBox assertion");
        end
        if (invalid_network_mode) begin
          $fatal(1, "INVALID_NETWORK scenario did not trip PredicateSwitchBox assertion");
        end
        if (invalid_fu_mode) begin
          $fatal(1, "INVALID_FU scenario did not trip PredicateSwitchBox assertion");
        end
        expect_bit("north we", north_pred_we, 1'b1);
        expect_pred("north bypass", north_pred_out, 1'b1);
        expect_bit("south we", south_pred_we, 1'b1);
        expect_pred("south bypass", south_pred_out, 1'b0);
        expect_bit("east we", east_pred_we, 1'b1);
        expect_pred("east bypass", east_pred_out, 1'b1);
        expect_bit("west we", west_pred_we, 1'b1);
        expect_pred("west bypass", west_pred_out, 1'b0);

        north_src <= PRED_ROUTE_SRC_FU_PRED;
        south_src <= PRED_ROUTE_SRC_RF_A;
        east_src <= PRED_ROUTE_SRC_RF_B;
        west_src <= PRED_ROUTE_SRC_CONST_TRUE;
      end
      2: begin
        expect_pred("north fu", north_pred_out, 1'b1);
        expect_pred("south rf_a", south_pred_out, 1'b0);
        expect_pred("east rf_b", east_pred_out, 1'b1);
        expect_pred("west const true", west_pred_out, 1'b1);

        north_src <= PRED_ROUTE_SRC_CONST_FALSE;
        south_src <= PRED_ROUTE_SRC_WEST_PRED_IN;
        east_src <= PRED_ROUTE_SRC_WEST_PRED_IN;
        west_src <= PRED_ROUTE_SRC_NORTH_PRED_IN;
      end
      3: begin
        expect_pred("north const false", north_pred_out, 1'b0);
        expect_pred("south fanout west", south_pred_out, 1'b0);
        expect_pred("east fanout west", east_pred_out, 1'b0);
        expect_pred("west north input", west_pred_out, 1'b1);

        north_we <= 1'b0;
        north_src <= PRED_ROUTE_SRC_NONE;
        south_we <= 1'b0;
        south_src <= PRED_ROUTE_SRC_NONE;
        east_we <= 1'b1;
        east_src <= PRED_ROUTE_SRC_CONST_TRUE;
        west_we <= 1'b0;
        west_src <= PRED_ROUTE_SRC_NONE;
      end
      4: begin
        expect_bit("north disabled we", north_pred_we, 1'b0);
        expect_bit("south disabled we", south_pred_we, 1'b0);
        expect_bit("east const true we", east_pred_we, 1'b1);
        expect_pred("east const true", east_pred_out, 1'b1);
        expect_bit("west disabled we", west_pred_we, 1'b0);
        $display("PredicateSwitchBox test passed");
        $finish;
      end
      default: begin
        $fatal(1, "PredicateSwitchBox test timed out");
      end
    endcase
  end
endmodule : pred_switchbox_tb
