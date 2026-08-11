// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_src_mux #(
  parameter int WIDTH = cgra_pkg::DATA_WIDTH
) (
  input  logic                 consume,
  input  cgra_pkg::data_src_e  src_sel,

  input  logic [WIDTH-1:0]     rf_a_data,
  input  logic [WIDTH-1:0]     rf_b_data,
  input  logic [WIDTH-1:0]     north_data_in,
  input  logic                 north_data_valid,
  input  logic [WIDTH-1:0]     south_data_in,
  input  logic                 south_data_valid,
  input  logic [WIDTH-1:0]     east_data_in,
  input  logic                 east_data_valid,
  input  logic [WIDTH-1:0]     west_data_in,
  input  logic                 west_data_valid,
  input  logic [WIDTH-1:0]     const_data,
  input  logic [WIDTH-1:0]     lsu_load_data,
  input  logic                 lsu_load_valid,

  output logic [WIDTH-1:0]     data_out,
  output logic                 src_valid
);
  always_comb begin
    data_out = '0;
    src_valid = 1'b0;

    unique case (src_sel)
      cgra_pkg::DATA_SRC_RF_A: begin
        data_out = rf_a_data;
        src_valid = 1'b1;
      end
      cgra_pkg::DATA_SRC_RF_B: begin
        data_out = rf_b_data;
        src_valid = 1'b1;
      end
      cgra_pkg::DATA_SRC_NORTH_DATA_IN: begin
        data_out = north_data_in;
        src_valid = north_data_valid;
      end
      cgra_pkg::DATA_SRC_SOUTH_DATA_IN: begin
        data_out = south_data_in;
        src_valid = south_data_valid;
      end
      cgra_pkg::DATA_SRC_EAST_DATA_IN: begin
        data_out = east_data_in;
        src_valid = east_data_valid;
      end
      cgra_pkg::DATA_SRC_WEST_DATA_IN: begin
        data_out = west_data_in;
        src_valid = west_data_valid;
      end
      cgra_pkg::DATA_SRC_CONST_DATA: begin
        data_out = const_data;
        src_valid = 1'b1;
      end
      cgra_pkg::DATA_SRC_LSU_LOAD_DATA: begin
        data_out = lsu_load_data;
        src_valid = lsu_load_valid;
      end
      cgra_pkg::DATA_SRC_ZERO: begin
        data_out = '0;
        src_valid = 1'b1;
      end
      default: begin
        data_out = '0;
        src_valid = 1'b0;
      end
    endcase
  end

`ifndef SYNTHESIS
  always_comb begin
    if (consume && !src_valid) begin
      $fatal(1, "Data source selected without valid data: src_sel=%0d", src_sel);
    end
  end
`endif
endmodule : data_src_mux
