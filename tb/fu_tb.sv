// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module fu_tb(input logic clk);
  import cgra_pkg::*;

  op_e op;
  logic [DATA_WIDTH-1:0] data_a;
  logic [DATA_WIDTH-1:0] data_b;
  logic [PRED_WIDTH-1:0] pred_p0;
  logic [PRED_WIDTH-1:0] pred_p1;
  logic [DATA_WIDTH-1:0] data_result;
  logic data_result_valid;
  logic [PRED_WIDTH-1:0] pred_result;
  logic pred_result_valid;
  int cycle;

  fu dut (
    .op(op),
    .data_a(data_a),
    .data_b(data_b),
    .pred_p0(pred_p0),
    .pred_p1(pred_p1),
    .data_result(data_result),
    .data_result_valid(data_result_valid),
    .pred_result(pred_result),
    .pred_result_valid(pred_result_valid)
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
    op = OP_NOP;
    data_a = '0;
    data_b = '0;
    pred_p0 = '0;
    pred_p1 = '0;
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    unique case (cycle)
      0: begin
        op <= OP_NOP;
      end
      1: begin
        expect_bit("NOP data valid", data_result_valid, 1'b0);
        expect_bit("NOP pred valid", pred_result_valid, 1'b0);
        op <= OP_PASS;
        data_a <= 32'h1234_abcd;
        data_b <= 32'hffff_ffff;
      end
      2: begin
        expect_data("PASS", data_result, 32'h1234_abcd);
        expect_bit("PASS data valid", data_result_valid, 1'b1);
        expect_bit("PASS pred valid", pred_result_valid, 1'b0);
        op <= OP_ADD;
        data_a <= 32'hffff_ffff;
        data_b <= 32'h0000_0002;
      end
      3: begin
        expect_data("ADD wrap", data_result, 32'h0000_0001);
        expect_bit("ADD data valid", data_result_valid, 1'b1);
        op <= OP_SUB;
        data_a <= 32'h0000_0000;
        data_b <= 32'h0000_0001;
      end
      4: begin
        expect_data("SUB wrap", data_result, 32'hffff_ffff);
        op <= OP_MUL;
        data_a <= 32'hffff_ffff;
        data_b <= 32'h0000_0002;
      end
      5: begin
        expect_data("MUL low 32", data_result, 32'hffff_fffe);
        op <= OP_AND;
        data_a <= 32'ha5a5_0f0f;
        data_b <= 32'hff00_3333;
      end
      6: begin
        expect_data("AND", data_result, 32'ha500_0303);
        op <= OP_OR;
        data_a <= 32'ha5a5_0f0f;
        data_b <= 32'h00ff_3333;
      end
      7: begin
        expect_data("OR", data_result, 32'ha5ff_3f3f);
        op <= OP_XOR;
        data_a <= 32'ha5a5_0f0f;
        data_b <= 32'h00ff_3333;
      end
      8: begin
        expect_data("XOR", data_result, 32'ha55a_3c3c);
        op <= OP_SHL;
        data_a <= 32'h0000_0001;
        data_b <= 32'h0000_0028;
      end
      9: begin
        expect_data("SHL low 5", data_result, 32'h0000_0100);
        op <= OP_LSHR;
        data_a <= 32'h8000_0000;
        data_b <= 32'h0000_0024;
      end
      10: begin
        expect_data("LSHR low 5", data_result, 32'h0800_0000);
        op <= OP_SELECT;
        data_a <= 32'h1111_1111;
        data_b <= 32'h2222_2222;
        pred_p0 <= 1'b1;
      end
      11: begin
        expect_data("SELECT true", data_result, 32'h1111_1111);
        op <= OP_SELECT;
        data_a <= 32'h3333_3333;
        data_b <= 32'h4444_4444;
        pred_p0 <= 1'b0;
      end
      12: begin
        expect_data("SELECT false", data_result, 32'h4444_4444);
        op <= OP_CMP_EQ;
        data_a <= 32'h0000_aaaa;
        data_b <= 32'h0000_aaaa;
      end
      13: begin
        expect_pred("CMP_EQ", pred_result, 1'b1);
        expect_bit("CMP_EQ data valid", data_result_valid, 1'b0);
        expect_bit("CMP_EQ pred valid", pred_result_valid, 1'b1);
        op <= OP_CMP_NE;
        data_a <= 32'h0000_aaaa;
        data_b <= 32'h0000_5555;
      end
      14: begin
        expect_pred("CMP_NE", pred_result, 1'b1);
        op <= OP_CMP_ULT;
        data_a <= 32'h0000_0001;
        data_b <= 32'hffff_ffff;
      end
      15: begin
        expect_pred("CMP_ULT unsigned", pred_result, 1'b1);
        op <= OP_CMP_ULE;
        data_a <= 32'hffff_ffff;
        data_b <= 32'h0000_0001;
      end
      16: begin
        expect_pred("CMP_ULE unsigned false", pred_result, 1'b0);
        op <= OP_PPASS;
        pred_p0 <= 1'b1;
        pred_p1 <= 1'b0;
      end
      17: begin
        expect_pred("PPASS", pred_result, 1'b1);
        expect_bit("PPASS data valid", data_result_valid, 1'b0);
        expect_bit("PPASS pred valid", pred_result_valid, 1'b1);
        op <= OP_PNOT;
        pred_p0 <= 1'b1;
      end
      18: begin
        expect_pred("PNOT", pred_result, 1'b0);
        op <= OP_PAND;
        pred_p0 <= 1'b1;
        pred_p1 <= 1'b0;
      end
      19: begin
        expect_pred("PAND", pred_result, 1'b0);
        op <= OP_POR;
        pred_p0 <= 1'b1;
        pred_p1 <= 1'b0;
      end
      20: begin
        expect_pred("POR", pred_result, 1'b1);
        $display("FU test passed");
        $finish;
      end
      default: begin
        $fatal(1, "FU test timed out");
      end
    endcase
  end
endmodule : fu_tb
