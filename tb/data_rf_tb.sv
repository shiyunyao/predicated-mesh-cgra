// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_rf_tb(input logic clk);
  import cgra_pkg::*;

  logic rst_n;
  logic ren_a;
  logic [DATA_RF_ADDR_WIDTH-1:0] raddr_a;
  logic [DATA_WIDTH-1:0] rdata_a;
  logic ren_b;
  logic [DATA_RF_ADDR_WIDTH-1:0] raddr_b;
  logic [DATA_WIDTH-1:0] rdata_b;
  logic w0_we;
  logic [DATA_RF_ADDR_WIDTH-1:0] w0_addr;
  logic [DATA_WIDTH-1:0] w0_data;
  logic w1_we;
  logic [DATA_RF_ADDR_WIDTH-1:0] w1_addr;
  logic [DATA_WIDTH-1:0] w1_data;
  int cycle;
  logic uninit_read_mode;
  logic read_write_hazard_mode;
  logic write_collision_mode;

  data_rf dut (
    .clk(clk),
    .rst_n(rst_n),
    .ren_a(ren_a),
    .raddr_a(raddr_a),
    .rdata_a(rdata_a),
    .ren_b(ren_b),
    .raddr_b(raddr_b),
    .rdata_b(rdata_b),
    .w0_we(w0_we),
    .w0_addr(w0_addr),
    .w0_data(w0_data),
    .w1_we(w1_we),
    .w1_addr(w1_addr),
    .w1_data(w1_data)
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

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    ren_a = 1'b0;
    raddr_a = '0;
    ren_b = 1'b0;
    raddr_b = '0;
    w0_we = 1'b0;
    w0_addr = '0;
    w0_data = '0;
    w1_we = 1'b0;
    w1_addr = '0;
    w1_data = '0;
    uninit_read_mode = ($test$plusargs("UNINIT_READ") != 0);
    read_write_hazard_mode = ($test$plusargs("READ_WRITE_HAZARD") != 0);
    write_collision_mode = ($test$plusargs("WRITE_COLLISION") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;

    rst_n <= 1'b1;
    w0_we <= 1'b0;
    w1_we <= 1'b0;
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
          $fatal(1, "UNINIT_READ scenario did not trip DataRF assertion");
        end
        w0_we <= 1'b1;
        w0_addr <= 4'h1;
        w0_data <= 32'h1111_aaaa;
        w1_we <= 1'b1;
        w1_addr <= 4'h2;
        w1_data <= 32'h2222_bbbb;
      end
      3: begin
        if (read_write_hazard_mode) begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
          w0_we <= 1'b1;
          w0_addr <= 4'h1;
          w0_data <= 32'h3333_cccc;
        end else if (write_collision_mode) begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
          w0_we <= 1'b1;
          w0_addr <= 4'h3;
          w0_data <= 32'h3333_cccc;
          w1_we <= 1'b1;
          w1_addr <= 4'h3;
          w1_data <= 32'h4444_dddd;
        end else begin
          ren_a <= 1'b1;
          ren_b <= 1'b1;
          raddr_a <= 4'h1;
          raddr_b <= 4'h2;
        end
      end
      4: begin
        if (read_write_hazard_mode) begin
          $fatal(1, "READ_WRITE_HAZARD scenario did not trip DataRF assertion");
        end
        if (write_collision_mode) begin
          $fatal(1, "WRITE_COLLISION scenario did not trip DataRF assertion");
        end
        expect_data("rdata_a", rdata_a, 32'h1111_aaaa);
        expect_data("rdata_b", rdata_b, 32'h2222_bbbb);
        w0_we <= 1'b1;
        w0_addr <= 4'h3;
        w0_data <= 32'h3333_cccc;
        w1_we <= 1'b1;
        w1_addr <= 4'h4;
        w1_data <= 32'h4444_dddd;
      end
      5: begin
        ren_a <= 1'b1;
        ren_b <= 1'b1;
        raddr_a <= 4'h3;
        raddr_b <= 4'h4;
      end
      6: begin
        expect_data("rdata_a second pair", rdata_a, 32'h3333_cccc);
        expect_data("rdata_b second pair", rdata_b, 32'h4444_dddd);
        $display("DataRF test passed");
        $finish;
      end
      default: begin
        $fatal(1, "DataRF test timed out");
      end
    endcase
  end
endmodule : data_rf_tb
