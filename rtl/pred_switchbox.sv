// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_switchbox #(
  parameter int WIDTH = cgra_pkg::PRED_WIDTH
) (
  input  logic                      north_we,
  input  cgra_pkg::pred_route_src_e north_src,
  input  logic                      south_we,
  input  cgra_pkg::pred_route_src_e south_src,
  input  logic                      east_we,
  input  cgra_pkg::pred_route_src_e east_src,
  input  logic                      west_we,
  input  cgra_pkg::pred_route_src_e west_src,

  input  logic [WIDTH-1:0]          north_pred_in,
  input  logic                      north_pred_valid,
  input  logic [WIDTH-1:0]          south_pred_in,
  input  logic                      south_pred_valid,
  input  logic [WIDTH-1:0]          east_pred_in,
  input  logic                      east_pred_valid,
  input  logic [WIDTH-1:0]          west_pred_in,
  input  logic                      west_pred_valid,
  input  logic [WIDTH-1:0]          fu_pred,
  input  logic                      fu_pred_valid,
  input  logic [WIDTH-1:0]          rf_a_pred,
  input  logic [WIDTH-1:0]          rf_b_pred,

  output logic                      north_pred_we,
  output logic [WIDTH-1:0]          north_pred_out,
  output logic                      south_pred_we,
  output logic [WIDTH-1:0]          south_pred_out,
  output logic                      east_pred_we,
  output logic [WIDTH-1:0]          east_pred_out,
  output logic                      west_pred_we,
  output logic [WIDTH-1:0]          west_pred_out
);
  logic north_src_valid;
  logic south_src_valid;
  logic east_src_valid;
  logic west_src_valid;

  always_comb begin
    north_pred_out = '0;
    north_src_valid = 1'b0;

    unique case (north_src)
      cgra_pkg::PRED_ROUTE_SRC_NONE: begin
        north_pred_out = '0;
        north_src_valid = 1'b0;
      end
      cgra_pkg::PRED_ROUTE_SRC_NORTH_PRED_IN: begin
        north_pred_out = north_pred_in;
        north_src_valid = north_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_SOUTH_PRED_IN: begin
        north_pred_out = south_pred_in;
        north_src_valid = south_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_EAST_PRED_IN: begin
        north_pred_out = east_pred_in;
        north_src_valid = east_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_WEST_PRED_IN: begin
        north_pred_out = west_pred_in;
        north_src_valid = west_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_FU_PRED: begin
        north_pred_out = fu_pred;
        north_src_valid = fu_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_A: begin
        north_pred_out = rf_a_pred;
        north_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_B: begin
        north_pred_out = rf_b_pred;
        north_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_TRUE: begin
        north_pred_out = {{(WIDTH-1){1'b0}}, 1'b1};
        north_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_FALSE: begin
        north_pred_out = '0;
        north_src_valid = 1'b1;
      end
      default: begin
        north_pred_out = '0;
        north_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    south_pred_out = '0;
    south_src_valid = 1'b0;

    unique case (south_src)
      cgra_pkg::PRED_ROUTE_SRC_NONE: begin
        south_pred_out = '0;
        south_src_valid = 1'b0;
      end
      cgra_pkg::PRED_ROUTE_SRC_NORTH_PRED_IN: begin
        south_pred_out = north_pred_in;
        south_src_valid = north_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_SOUTH_PRED_IN: begin
        south_pred_out = south_pred_in;
        south_src_valid = south_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_EAST_PRED_IN: begin
        south_pred_out = east_pred_in;
        south_src_valid = east_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_WEST_PRED_IN: begin
        south_pred_out = west_pred_in;
        south_src_valid = west_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_FU_PRED: begin
        south_pred_out = fu_pred;
        south_src_valid = fu_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_A: begin
        south_pred_out = rf_a_pred;
        south_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_B: begin
        south_pred_out = rf_b_pred;
        south_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_TRUE: begin
        south_pred_out = {{(WIDTH-1){1'b0}}, 1'b1};
        south_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_FALSE: begin
        south_pred_out = '0;
        south_src_valid = 1'b1;
      end
      default: begin
        south_pred_out = '0;
        south_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    east_pred_out = '0;
    east_src_valid = 1'b0;

    unique case (east_src)
      cgra_pkg::PRED_ROUTE_SRC_NONE: begin
        east_pred_out = '0;
        east_src_valid = 1'b0;
      end
      cgra_pkg::PRED_ROUTE_SRC_NORTH_PRED_IN: begin
        east_pred_out = north_pred_in;
        east_src_valid = north_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_SOUTH_PRED_IN: begin
        east_pred_out = south_pred_in;
        east_src_valid = south_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_EAST_PRED_IN: begin
        east_pred_out = east_pred_in;
        east_src_valid = east_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_WEST_PRED_IN: begin
        east_pred_out = west_pred_in;
        east_src_valid = west_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_FU_PRED: begin
        east_pred_out = fu_pred;
        east_src_valid = fu_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_A: begin
        east_pred_out = rf_a_pred;
        east_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_B: begin
        east_pred_out = rf_b_pred;
        east_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_TRUE: begin
        east_pred_out = {{(WIDTH-1){1'b0}}, 1'b1};
        east_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_FALSE: begin
        east_pred_out = '0;
        east_src_valid = 1'b1;
      end
      default: begin
        east_pred_out = '0;
        east_src_valid = 1'b0;
      end
    endcase
  end

  always_comb begin
    west_pred_out = '0;
    west_src_valid = 1'b0;

    unique case (west_src)
      cgra_pkg::PRED_ROUTE_SRC_NONE: begin
        west_pred_out = '0;
        west_src_valid = 1'b0;
      end
      cgra_pkg::PRED_ROUTE_SRC_NORTH_PRED_IN: begin
        west_pred_out = north_pred_in;
        west_src_valid = north_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_SOUTH_PRED_IN: begin
        west_pred_out = south_pred_in;
        west_src_valid = south_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_EAST_PRED_IN: begin
        west_pred_out = east_pred_in;
        west_src_valid = east_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_WEST_PRED_IN: begin
        west_pred_out = west_pred_in;
        west_src_valid = west_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_FU_PRED: begin
        west_pred_out = fu_pred;
        west_src_valid = fu_pred_valid;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_A: begin
        west_pred_out = rf_a_pred;
        west_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_RF_B: begin
        west_pred_out = rf_b_pred;
        west_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_TRUE: begin
        west_pred_out = {{(WIDTH-1){1'b0}}, 1'b1};
        west_src_valid = 1'b1;
      end
      cgra_pkg::PRED_ROUTE_SRC_CONST_FALSE: begin
        west_pred_out = '0;
        west_src_valid = 1'b1;
      end
      default: begin
        west_pred_out = '0;
        west_src_valid = 1'b0;
      end
    endcase
  end

  assign north_pred_we = north_we;
  assign south_pred_we = south_we;
  assign east_pred_we = east_we;
  assign west_pred_we = west_we;

`ifndef SYNTHESIS
  always_comb begin
    if (north_we && !north_src_valid) begin
      $fatal(1, "PredicateSwitchBox north route selected invalid source: src=%0d", north_src);
    end
    if (south_we && !south_src_valid) begin
      $fatal(1, "PredicateSwitchBox south route selected invalid source: src=%0d", south_src);
    end
    if (east_we && !east_src_valid) begin
      $fatal(1, "PredicateSwitchBox east route selected invalid source: src=%0d", east_src);
    end
    if (west_we && !west_src_valid) begin
      $fatal(1, "PredicateSwitchBox west route selected invalid source: src=%0d", west_src);
    end
  end
`endif
endmodule : pred_switchbox
