// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_rf_tb(input logic clk);
  import cgra_pkg::*;

  logic rst_n;
  logic ren_a;
  logic [PRED_RF_ADDR_WIDTH-1:0] raddr_a;
  logic [PRED_WIDTH-1:0] rdata_a;
  logic ren_b;
  logic [PRED_RF_ADDR_WIDTH-1:0] raddr_b;
  logic [PRED_WIDTH-1:0] rdata_b;
  logic pw0_we;
  logic [PRED_RF_ADDR_WIDTH-1:0] pw0_addr;
  logic [PRED_WIDTH-1:0] pw0_data;
  logic pw1_we;
  logic [PRED_RF_ADDR_WIDTH-1:0] pw1_addr;
  logic [PRED_WIDTH-1:0] pw1_data;
  int cycle;
  logic uninit_read_mode;
  logic read_write_hazard_mode;
  logic write_collision_mode;

  pred_rf dut (
    .clk(clk),
    .rst_n(rst_n),
    .ren_a(ren_a),
    .raddr_a(raddr_a),
    .rdata_a(rdata_a),
    .ren_b(ren_b),
    .raddr_b(raddr_b),
    .rdata_b(rdata_b),
    .pw0_we(pw0_we),
    .pw0_addr(pw0_addr),
    .pw0_data(pw0_data),
    .pw1_we(pw1_we),
    .pw1_addr(pw1_addr),
    .pw1_data(pw1_data)
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

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    ren_a = 1'b0;
    raddr_a = '0;
    ren_b = 1'b0;
    raddr_b = '0;
    pw0_we = 1'b0;
    pw0_addr = '0;
    pw0_data = '0;
    pw1_we = 1'b0;
    pw1_addr = '0;
    pw1_data = '0;
    uninit_read_mode = ($test$plusargs("UNINIT_READ") != 0);
    read_write_hazard_mode = ($test$plusargs("READ_WRITE_HAZARD") != 0);
    write_collision_mode = ($test$plusargs("WRITE_COLLISION") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    rst_n <= 1'b1;
    pw0_we <= 1'b0;
    pw1_we <= 1'b0;
    ren_a <= 1'b0;
    ren_b <= 1'b0;
    raddr_a <= 4'h1;
    raddr_b <= 4'h2;

    unique case (cycle)
      0: begin
        rst_n <= 1'b0;
      end
      1: begin
        if (uninit_read_mode) begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
        end
      end
      2: begin
        if (uninit_read_mode) begin
          $fatal(1, "UNINIT_READ scenario did not trip PredicateRF assertion");
        end
        pw0_we <= 1'b1;
        pw0_addr <= 4'h1;
        pw0_data <= 1'b1;
        pw1_we <= 1'b1;
        pw1_addr <= 4'h2;
        pw1_data <= 1'b0;
      end
      3: begin
        if (read_write_hazard_mode) begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
          pw0_we <= 1'b1;
          pw0_addr <= 4'h1;
          pw0_data <= 1'b0;
        end else if (write_collision_mode) begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
          pw0_we <= 1'b1;
          pw0_addr <= 4'h3;
          pw0_data <= 1'b0;
          pw1_we <= 1'b1;
          pw1_addr <= 4'h3;
          pw1_data <= 1'b1;
        end else begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
        end
      end
      4: begin
        if (read_write_hazard_mode) begin
          $fatal(1, "READ_WRITE_HAZARD scenario did not trip PredicateRF assertion");
        end
        if (write_collision_mode) begin
          $fatal(1, "WRITE_COLLISION scenario did not trip PredicateRF assertion");
        end
        expect_pred("rdata_a", rdata_a, 1'b1);
        expect_pred("rdata_b", rdata_b, 1'b0);
        pw0_we <= 1'b1;
        pw0_addr <= 4'h3;
        pw0_data <= 1'b0;
        pw1_we <= 1'b1;
        pw1_addr <= 4'h4;
        pw1_data <= 1'b1;
      end
      5: begin
        ren_a <= 1'b1;
        ren_b <= 1'b1;
        raddr_a <= 4'h3;
        raddr_b <= 4'h4;
      end
      6: begin
        expect_pred("rdata_a second pair", rdata_a, 1'b0);
        expect_pred("rdata_b second pair", rdata_b, 1'b1);
        $display("PredicateRF test passed");
        $finish;
      end
      default: begin
        $fatal(1, "PredicateRF test timed out");
      end
    endcase
  end
endmodule : pred_rf_tb
