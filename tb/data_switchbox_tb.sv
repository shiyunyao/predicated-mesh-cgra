// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_switchbox_tb(input logic clk);
  import cgra_pkg::*;

  logic north_we;
  data_route_src_e north_src;
  logic south_we;
  data_route_src_e south_src;
  logic east_we;
  data_route_src_e east_src;
  logic west_we;
  data_route_src_e west_src;
  logic [DATA_WIDTH-1:0] north_data_in;
  logic north_data_valid;
  logic [DATA_WIDTH-1:0] south_data_in;
  logic south_data_valid;
  logic [DATA_WIDTH-1:0] east_data_in;
  logic east_data_valid;
  logic [DATA_WIDTH-1:0] west_data_in;
  logic west_data_valid;
  logic [DATA_WIDTH-1:0] fu_data;
  logic fu_data_valid;
  logic [DATA_WIDTH-1:0] rf_a_data;
  logic [DATA_WIDTH-1:0] rf_b_data;
  logic [DATA_WIDTH-1:0] const_data;
  logic [DATA_WIDTH-1:0] lsu_load_data;
  logic lsu_load_valid;
  logic north_data_we;
  logic [DATA_WIDTH-1:0] north_data_out;
  logic south_data_we;
  logic [DATA_WIDTH-1:0] south_data_out;
  logic east_data_we;
  logic [DATA_WIDTH-1:0] east_data_out;
  logic west_data_we;
  logic [DATA_WIDTH-1:0] west_data_out;
  int cycle;
  logic invalid_none_mode;
  logic invalid_network_mode;
  logic invalid_fu_mode;
  logic invalid_lsu_mode;

  data_switchbox dut (
    .north_we(north_we),
    .north_src(north_src),
    .south_we(south_we),
    .south_src(south_src),
    .east_we(east_we),
    .east_src(east_src),
    .west_we(west_we),
    .west_src(west_src),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .fu_data(fu_data),
    .fu_data_valid(fu_data_valid),
    .rf_a_data(rf_a_data),
    .rf_b_data(rf_b_data),
    .const_data(const_data),
    .lsu_load_data(lsu_load_data),
    .lsu_load_valid(lsu_load_valid),
    .north_data_we(north_data_we),
    .north_data_out(north_data_out),
    .south_data_we(south_data_we),
    .south_data_out(south_data_out),
    .east_data_we(east_data_we),
    .east_data_out(east_data_out),
    .west_data_we(west_data_we),
    .west_data_out(west_data_out)
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
    north_we = 1'b0;
    north_src = DATA_ROUTE_SRC_NONE;
    south_we = 1'b0;
    south_src = DATA_ROUTE_SRC_NONE;
    east_we = 1'b0;
    east_src = DATA_ROUTE_SRC_NONE;
    west_we = 1'b0;
    west_src = DATA_ROUTE_SRC_NONE;
    north_data_in = 32'h1111_aaaa;
    north_data_valid = 1'b1;
    south_data_in = 32'h2222_bbbb;
    south_data_valid = 1'b1;
    east_data_in = 32'h3333_cccc;
    east_data_valid = 1'b1;
    west_data_in = 32'h4444_dddd;
    west_data_valid = 1'b1;
    fu_data = 32'h5555_eeee;
    fu_data_valid = 1'b1;
    rf_a_data = 32'h6666_ffff;
    rf_b_data = 32'h7777_1234;
    const_data = 32'h8888_5678;
    lsu_load_data = 32'h9999_9abc;
    lsu_load_valid = 1'b1;
    invalid_none_mode = ($test$plusargs("INVALID_NONE") != 0);
    invalid_network_mode = ($test$plusargs("INVALID_NETWORK") != 0);
    invalid_fu_mode = ($test$plusargs("INVALID_FU") != 0);
    invalid_lsu_mode = ($test$plusargs("INVALID_LSU") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    unique case (cycle)
      0: begin
        if (invalid_none_mode) begin
          north_we <= 1'b1;
          north_src <= DATA_ROUTE_SRC_NONE;
        end else if (invalid_network_mode) begin
          south_we <= 1'b1;
          south_src <= DATA_ROUTE_SRC_NORTH_DATA_IN;
          north_data_valid <= 1'b0;
        end else if (invalid_fu_mode) begin
          east_we <= 1'b1;
          east_src <= DATA_ROUTE_SRC_FU_DATA;
          fu_data_valid <= 1'b0;
        end else if (invalid_lsu_mode) begin
          west_we <= 1'b1;
          west_src <= DATA_ROUTE_SRC_LSU_LOAD_DATA;
          lsu_load_valid <= 1'b0;
        end else begin
          north_we <= 1'b1;
          north_src <= DATA_ROUTE_SRC_NORTH_DATA_IN;
          south_we <= 1'b1;
          south_src <= DATA_ROUTE_SRC_SOUTH_DATA_IN;
          east_we <= 1'b1;
          east_src <= DATA_ROUTE_SRC_EAST_DATA_IN;
          west_we <= 1'b1;
          west_src <= DATA_ROUTE_SRC_WEST_DATA_IN;
        end
      end
      1: begin
        if (invalid_none_mode) begin
          $fatal(1, "INVALID_NONE scenario did not trip DataSwitchBox assertion");
        end
        if (invalid_network_mode) begin
          $fatal(1, "INVALID_NETWORK scenario did not trip DataSwitchBox assertion");
        end
        if (invalid_fu_mode) begin
          $fatal(1, "INVALID_FU scenario did not trip DataSwitchBox assertion");
        end
        if (invalid_lsu_mode) begin
          $fatal(1, "INVALID_LSU scenario did not trip DataSwitchBox assertion");
        end
        expect_bit("north we", north_data_we, 1'b1);
        expect_data("north bypass", north_data_out, 32'h1111_aaaa);
        expect_bit("south we", south_data_we, 1'b1);
        expect_data("south bypass", south_data_out, 32'h2222_bbbb);
        expect_bit("east we", east_data_we, 1'b1);
        expect_data("east bypass", east_data_out, 32'h3333_cccc);
        expect_bit("west we", west_data_we, 1'b1);
        expect_data("west bypass", west_data_out, 32'h4444_dddd);

        north_src <= DATA_ROUTE_SRC_FU_DATA;
        south_src <= DATA_ROUTE_SRC_RF_A;
        east_src <= DATA_ROUTE_SRC_RF_B;
        west_src <= DATA_ROUTE_SRC_CONST_DATA;
      end
      2: begin
        expect_data("north fu", north_data_out, 32'h5555_eeee);
        expect_data("south rf_a", south_data_out, 32'h6666_ffff);
        expect_data("east rf_b", east_data_out, 32'h7777_1234);
        expect_data("west const", west_data_out, 32'h8888_5678);

        north_src <= DATA_ROUTE_SRC_LSU_LOAD_DATA;
        south_src <= DATA_ROUTE_SRC_ZERO;
        east_src <= DATA_ROUTE_SRC_WEST_DATA_IN;
        west_src <= DATA_ROUTE_SRC_WEST_DATA_IN;
      end
      3: begin
        expect_data("north lsu", north_data_out, 32'h9999_9abc);
        expect_data("south zero", south_data_out, 32'h0000_0000);
        expect_data("east fanout west", east_data_out, 32'h4444_dddd);
        expect_data("west fanout west", west_data_out, 32'h4444_dddd);

        north_we <= 1'b0;
        north_src <= DATA_ROUTE_SRC_NONE;
        south_we <= 1'b0;
        south_src <= DATA_ROUTE_SRC_NONE;
        east_we <= 1'b1;
        east_src <= DATA_ROUTE_SRC_ZERO;
        west_we <= 1'b0;
        west_src <= DATA_ROUTE_SRC_NONE;
      end
      4: begin
        expect_bit("north disabled we", north_data_we, 1'b0);
        expect_bit("south disabled we", south_data_we, 1'b0);
        expect_bit("east zero we", east_data_we, 1'b1);
        expect_data("east zero", east_data_out, 32'h0000_0000);
        expect_bit("west disabled we", west_data_we, 1'b0);
        $display("DataSwitchBox test passed");
        $finish;
      end
      default: begin
        $fatal(1, "DataSwitchBox test timed out");
      end
    endcase
  end
endmodule : data_switchbox_tb
