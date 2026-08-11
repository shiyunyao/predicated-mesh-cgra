// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module fu #(
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int PRED_WIDTH = cgra_pkg::PRED_WIDTH
) (
  input  cgra_pkg::op_e        op,
  input  logic [DATA_WIDTH-1:0] data_a,
  input  logic [DATA_WIDTH-1:0] data_b,
  input  logic [PRED_WIDTH-1:0] pred_p0,
  input  logic [PRED_WIDTH-1:0] pred_p1,

  output logic [DATA_WIDTH-1:0] data_result,
  output logic                  data_result_valid,
  output logic [PRED_WIDTH-1:0] pred_result,
  output logic                  pred_result_valid
);
  always_comb begin
    data_result = '0;
    data_result_valid = 1'b0;
    pred_result = '0;
    pred_result_valid = 1'b0;

    unique case (op)
      cgra_pkg::OP_NOP: begin
      end
      cgra_pkg::OP_PASS: begin
        data_result = data_a;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_ADD: begin
        data_result = data_a + data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_SUB: begin
        data_result = data_a - data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_MUL: begin
        data_result = data_a * data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_AND: begin
        data_result = data_a & data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_OR: begin
        data_result = data_a | data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_XOR: begin
        data_result = data_a ^ data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_SHL: begin
        data_result = data_a << data_b[4:0];
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_LSHR: begin
        data_result = data_a >> data_b[4:0];
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_SELECT: begin
        data_result = pred_p0[0] ? data_a : data_b;
        data_result_valid = 1'b1;
      end
      cgra_pkg::OP_CMP_EQ: begin
        pred_result = (data_a == data_b);
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_CMP_NE: begin
        pred_result = (data_a != data_b);
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_CMP_ULT: begin
        pred_result = (data_a < data_b);
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_CMP_ULE: begin
        pred_result = (data_a <= data_b);
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_PPASS: begin
        pred_result = pred_p0;
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_PNOT: begin
        pred_result = ~pred_p0;
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_PAND: begin
        pred_result = pred_p0 & pred_p1;
        pred_result_valid = 1'b1;
      end
      cgra_pkg::OP_POR: begin
        pred_result = pred_p0 | pred_p1;
        pred_result_valid = 1'b1;
      end
      default: begin
      end
    endcase
  end
endmodule : fu
