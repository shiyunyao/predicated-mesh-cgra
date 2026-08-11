// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module data_rf #(
  parameter int WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int DEPTH = cgra_pkg::DATA_RF_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::DATA_RF_ADDR_WIDTH
) (
  input  logic                  clk,
  input  logic                  rst_n,

  input  logic                  ren_a,
  input  logic [ADDR_WIDTH-1:0] raddr_a,
  output logic [WIDTH-1:0]      rdata_a,
  input  logic                  ren_b,
  input  logic [ADDR_WIDTH-1:0] raddr_b,
  output logic [WIDTH-1:0]      rdata_b,

  input  logic                  w0_we,
  input  logic [ADDR_WIDTH-1:0] w0_addr,
  input  logic [WIDTH-1:0]      w0_data,
  input  logic                  w1_we,
  input  logic [ADDR_WIDTH-1:0] w1_addr,
  input  logic [WIDTH-1:0]      w1_data
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
        $fatal(1, "DataRF uninitialized read on port A: addr=%0d", raddr_a);
      end
      if (ren_b && !initialized[raddr_b]) begin
        $fatal(1, "DataRF uninitialized read on port B: addr=%0d", raddr_b);
      end
      if (w0_we && ((ren_a && (w0_addr == raddr_a)) || (ren_b && (w0_addr == raddr_b)))) begin
        $fatal(1, "DataRF same-cycle read/write hazard on W0: addr=%0d", w0_addr);
      end
      if (w1_we && ((ren_a && (w1_addr == raddr_a)) || (ren_b && (w1_addr == raddr_b)))) begin
        $fatal(1, "DataRF same-cycle read/write hazard on W1: addr=%0d", w1_addr);
      end
      if (w0_we && w1_we && (w0_addr == w1_addr)) begin
        $fatal(1, "DataRF W0/W1 write collision: addr=%0d", w0_addr);
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
      if (w0_we) begin
        mem[w0_addr] <= w0_data;
`ifndef SYNTHESIS
        initialized[w0_addr] <= 1'b1;
`endif
      end
      if (w1_we) begin
        mem[w1_addr] <= w1_data;
`ifndef SYNTHESIS
        initialized[w1_addr] <= 1'b1;
`endif
      end
    end
  end
endmodule : data_rf
