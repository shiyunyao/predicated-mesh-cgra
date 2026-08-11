// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_src_mux #(
  parameter int WIDTH = cgra_pkg::PRED_WIDTH
) (
  input  logic                 consume,
  input  cgra_pkg::pred_src_e  src_sel,

  input  logic [WIDTH-1:0]     rf_a_pred,
  input  logic [WIDTH-1:0]     rf_b_pred,
  input  logic [WIDTH-1:0]     north_pred_in,
  input  logic                 north_pred_valid,
  input  logic [WIDTH-1:0]     south_pred_in,
  input  logic                 south_pred_valid,
  input  logic [WIDTH-1:0]     east_pred_in,
  input  logic                 east_pred_valid,
  input  logic [WIDTH-1:0]     west_pred_in,
  input  logic                 west_pred_valid,

  output logic [WIDTH-1:0]     pred_out,
  output logic                 src_valid
);
  always_comb begin
    pred_out = '0;
    src_valid = 1'b0;

    unique case (src_sel)
      cgra_pkg::PRED_SRC_RF_A: begin
        pred_out = rf_a_pred;
        src_valid = 1'b1;
      end
      cgra_pkg::PRED_SRC_RF_B: begin
        pred_out = rf_b_pred;
        src_valid = 1'b1;
      end
      cgra_pkg::PRED_SRC_NORTH_PRED_IN: begin
        pred_out = north_pred_in;
        src_valid = north_pred_valid;
      end
      cgra_pkg::PRED_SRC_SOUTH_PRED_IN: begin
        pred_out = south_pred_in;
        src_valid = south_pred_valid;
      end
      cgra_pkg::PRED_SRC_EAST_PRED_IN: begin
        pred_out = east_pred_in;
        src_valid = east_pred_valid;
      end
      cgra_pkg::PRED_SRC_WEST_PRED_IN: begin
        pred_out = west_pred_in;
        src_valid = west_pred_valid;
      end
      cgra_pkg::PRED_SRC_CONST_TRUE: begin
        pred_out = {{(WIDTH-1){1'b0}}, 1'b1};
        src_valid = 1'b1;
      end
      cgra_pkg::PRED_SRC_CONST_FALSE: begin
        pred_out = '0;
        src_valid = 1'b1;
      end
      default: begin
        pred_out = '0;
        src_valid = 1'b0;
      end
    endcase
  end

`ifndef SYNTHESIS
  always_comb begin
    if (consume && !src_valid) begin
      $fatal(1, "Predicate source selected without valid predicate: src_sel=%0d", src_sel);
    end
  end
`endif
endmodule : pred_src_mux
