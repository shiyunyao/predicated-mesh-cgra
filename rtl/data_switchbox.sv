// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_switchbox #(
  parameter int WIDTH = cgra_pkg::DATA_WIDTH
) (
  input  logic                      north_we,
  input  cgra_pkg::data_route_src_e north_src,
  input  logic                      south_we,
  input  cgra_pkg::data_route_src_e south_src,
  input  logic                      east_we,
  input  cgra_pkg::data_route_src_e east_src,
  input  logic                      west_we,
  input  cgra_pkg::data_route_src_e west_src,

  input  logic [WIDTH-1:0]          north_data_in,
  input  logic                      north_data_valid,
  input  logic [WIDTH-1:0]          south_data_in,
  input  logic                      south_data_valid,
  input  logic [WIDTH-1:0]          east_data_in,
  input  logic                      east_data_valid,
  input  logic [WIDTH-1:0]          west_data_in,
  input  logic                      west_data_valid,
  input  logic [WIDTH-1:0]          fu_data,
  input  logic                      fu_data_valid,
  input  logic [WIDTH-1:0]          rf_a_data,
  input  logic [WIDTH-1:0]          rf_b_data,
  input  logic [WIDTH-1:0]          const_data,
  input  logic [WIDTH-1:0]          lsu_load_data,
  input  logic                      lsu_load_valid,

  output logic                      north_data_we,
  output logic [WIDTH-1:0]          north_data_out,
  output logic                      south_data_we,
  output logic [WIDTH-1:0]          south_data_out,
  output logic                      east_data_we,
  output logic [WIDTH-1:0]          east_data_out,
  output logic                      west_data_we,
  output logic [WIDTH-1:0]          west_data_out
);
  logic north_src_valid;
  logic south_src_valid;
  logic east_src_valid;
  logic west_src_valid;

  always_comb begin
    north_data_out = '0;
    north_src_valid = 1'b0;

    unique case (north_src)
      cgra_pkg::DATA_ROUTE_SRC_NONE: begin
        north_data_out = '0;
        north_src_valid = 1'b0;
      end
      cgra_pkg::DATA_ROUTE_SRC_NORTH_DATA_IN: begin
        north_data_out = north_data_in;
        north_src_valid = north_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_SOUTH_DATA_IN: begin
        north_data_out = south_data_in;
        north_src_valid = south_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_EAST_DATA_IN: begin
        north_data_out = east_data_in;
        north_src_valid = east_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_WEST_DATA_IN: begin
        north_data_out = west_data_in;
        north_src_valid = west_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_FU_DATA: begin
        north_data_out = fu_data;
        north_src_valid = fu_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_A: begin
        north_data_out = rf_a_data;
        north_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_B: begin
        north_data_out = rf_b_data;
        north_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_CONST_DATA: begin
        north_data_out = const_data;
        north_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_LSU_LOAD_DATA: begin
        north_data_out = lsu_load_data;
        north_src_valid = lsu_load_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_ZERO: begin
        north_data_out = '0;
        north_src_valid = 1'b1;
      end
      default: begin
        north_data_out = '0;
        north_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    south_data_out = '0;
    south_src_valid = 1'b0;

    unique case (south_src)
      cgra_pkg::DATA_ROUTE_SRC_NONE: begin
        south_data_out = '0;
        south_src_valid = 1'b0;
      end
      cgra_pkg::DATA_ROUTE_SRC_NORTH_DATA_IN: begin
        south_data_out = north_data_in;
        south_src_valid = north_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_SOUTH_DATA_IN: begin
        south_data_out = south_data_in;
        south_src_valid = south_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_EAST_DATA_IN: begin
        south_data_out = east_data_in;
        south_src_valid = east_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_WEST_DATA_IN: begin
        south_data_out = west_data_in;
        south_src_valid = west_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_FU_DATA: begin
        south_data_out = fu_data;
        south_src_valid = fu_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_A: begin
        south_data_out = rf_a_data;
        south_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_B: begin
        south_data_out = rf_b_data;
        south_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_CONST_DATA: begin
        south_data_out = const_data;
        south_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_LSU_LOAD_DATA: begin
        south_data_out = lsu_load_data;
        south_src_valid = lsu_load_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_ZERO: begin
        south_data_out = '0;
        south_src_valid = 1'b1;
      end
      default: begin
        south_data_out = '0;
        south_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    east_data_out = '0;
    east_src_valid = 1'b0;

    unique case (east_src)
      cgra_pkg::DATA_ROUTE_SRC_NONE: begin
        east_data_out = '0;
        east_src_valid = 1'b0;
      end
      cgra_pkg::DATA_ROUTE_SRC_NORTH_DATA_IN: begin
        east_data_out = north_data_in;
        east_src_valid = north_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_SOUTH_DATA_IN: begin
        east_data_out = south_data_in;
        east_src_valid = south_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_EAST_DATA_IN: begin
        east_data_out = east_data_in;
        east_src_valid = east_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_WEST_DATA_IN: begin
        east_data_out = west_data_in;
        east_src_valid = west_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_FU_DATA: begin
        east_data_out = fu_data;
        east_src_valid = fu_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_A: begin
        east_data_out = rf_a_data;
        east_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_B: begin
        east_data_out = rf_b_data;
        east_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_CONST_DATA: begin
        east_data_out = const_data;
        east_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_LSU_LOAD_DATA: begin
        east_data_out = lsu_load_data;
        east_src_valid = lsu_load_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_ZERO: begin
        east_data_out = '0;
        east_src_valid = 1'b1;
      end
      default: begin
        east_data_out = '0;
        east_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    west_data_out = '0;
    west_src_valid = 1'b0;

    unique case (west_src)
      cgra_pkg::DATA_ROUTE_SRC_NONE: begin
        west_data_out = '0;
        west_src_valid = 1'b0;
      end
      cgra_pkg::DATA_ROUTE_SRC_NORTH_DATA_IN: begin
        west_data_out = north_data_in;
        west_src_valid = north_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_SOUTH_DATA_IN: begin
        west_data_out = south_data_in;
        west_src_valid = south_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_EAST_DATA_IN: begin
        west_data_out = east_data_in;
        west_src_valid = east_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_WEST_DATA_IN: begin
        west_data_out = west_data_in;
        west_src_valid = west_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_FU_DATA: begin
        west_data_out = fu_data;
        west_src_valid = fu_data_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_A: begin
        west_data_out = rf_a_data;
        west_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_RF_B: begin
        west_data_out = rf_b_data;
        west_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_CONST_DATA: begin
        west_data_out = const_data;
        west_src_valid = 1'b1;
      end
      cgra_pkg::DATA_ROUTE_SRC_LSU_LOAD_DATA: begin
        west_data_out = lsu_load_data;
        west_src_valid = lsu_load_valid;
      end
      cgra_pkg::DATA_ROUTE_SRC_ZERO: begin
        west_data_out = '0;
        west_src_valid = 1'b1;
      end
      default: begin
        west_data_out = '0;
        west_src_valid = 1'b0;
      end
    endcase
  end

  assign north_data_we = north_we;
  assign south_data_we = south_we;
  assign east_data_we = east_we;
  assign west_data_we = west_we;

`ifndef SYNTHESIS
  always_comb begin
    if (north_we && !north_src_valid) begin
      $fatal(1, "DataSwitchBox north route selected invalid source: src=%0d", north_src);
    end
    if (south_we && !south_src_valid) begin
      $fatal(1, "DataSwitchBox south route selected invalid source: src=%0d", south_src);
    end
    if (east_we && !east_src_valid) begin
      $fatal(1, "DataSwitchBox east route selected invalid source: src=%0d", east_src);
    end
    if (west_we && !west_src_valid) begin
      $fatal(1, "DataSwitchBox west route selected invalid source: src=%0d", west_src);
    end
  end
`endif
endmodule : data_switchbox
