// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module pred_rf #(
  parameter int WIDTH = cgra_pkg::PRED_WIDTH,
  parameter int DEPTH = cgra_pkg::PRED_RF_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::PRED_RF_ADDR_WIDTH
) (
  input  logic                  clk,
  input  logic                  rst_n,

  input  logic                  ren_a,
  input  logic [ADDR_WIDTH-1:0] raddr_a,
  output logic [WIDTH-1:0]      rdata_a,
  input  logic                  ren_b,
  input  logic [ADDR_WIDTH-1:0] raddr_b,
  output logic [WIDTH-1:0]      rdata_b,

  input  logic                  pw0_we,
  input  logic [ADDR_WIDTH-1:0] pw0_addr,
  input  logic [WIDTH-1:0]      pw0_data,
  input  logic                  pw1_we,
  input  logic [ADDR_WIDTH-1:0] pw1_addr,
  input  logic [WIDTH-1:0]      pw1_data
);
  logic [WIDTH-1:0] mem [DEPTH];

`ifndef SYNTHESIS
  logic initialized [DEPTH];
`endif

  assign rdata_a = mem[raddr_a];
  assign rdata_b = mem[raddr_b];

`ifndef SYNTHESIS
  always_comb begin
    if (!rst_n) begin
    end else begin
      if (ren_a && !initialized[raddr_a]) begin
        $fatal(1, "PredicateRF uninitialized read on port A: addr=%0d", raddr_a);
      end
      if (ren_b && !initialized[raddr_b]) begin
        $fatal(1, "PredicateRF uninitialized read on port B: addr=%0d", raddr_b);
      end
      if (pw0_we && ((ren_a && (pw0_addr == raddr_a)) || (ren_b && (pw0_addr == raddr_b)))) begin
        $fatal(1, "PredicateRF same-cycle read/write hazard on PW0: addr=%0d", pw0_addr);
      end
      if (pw1_we && ((ren_a && (pw1_addr == raddr_a)) || (ren_b && (pw1_addr == raddr_b)))) begin
        $fatal(1, "PredicateRF same-cycle read/write hazard on PW1: addr=%0d", pw1_addr);
      end
      if (pw0_we && pw1_we && (pw0_addr == pw1_addr)) begin
        $fatal(1, "PredicateRF PW0/PW1 write collision: addr=%0d", pw0_addr);
      end
    end
  end
`endif

  always_ff @(posedge clk) begin
    if (!rst_n) begin
`ifndef SYNTHESIS
      for (int i = 0; i < DEPTH; i++) begin
        initialized[i] <= 1'b0;
      end
`endif
    end else begin
      if (pw0_we) begin
        mem[pw0_addr] <= pw0_data;
`ifndef SYNTHESIS
        initialized[pw0_addr] <= 1'b1;
`endif
      end
      if (pw1_we) begin
        mem[pw1_addr] <= pw1_data;
`ifndef SYNTHESIS
        initialized[pw1_addr] <= 1'b1;
`endif
      end
    end
  end
endmodule : pred_rf
