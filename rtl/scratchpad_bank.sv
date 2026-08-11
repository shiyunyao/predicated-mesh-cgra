// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module scratchpad_bank #(
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int DEPTH = cgra_pkg::SCRATCH_BANK_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::SCRATCH_ADDR_WIDTH
) (
  input  logic                  clk,

  input  logic                  write_en,
  input  logic [ADDR_WIDTH-1:0] write_addr,
  input  logic [DATA_WIDTH-1:0] write_data,

  input  logic                  cfg_write_en,
  input  logic [ADDR_WIDTH-1:0] cfg_write_addr,
  input  logic [DATA_WIDTH-1:0] cfg_write_data,

  input  logic [ADDR_WIDTH-1:0] read_addr,
  output logic [DATA_WIDTH-1:0] read_data
);
  logic [DATA_WIDTH-1:0] mem [DEPTH];

  assign read_data = mem[read_addr];

  always_ff @(posedge clk) begin
    if (write_en) begin
      mem[write_addr] <= write_data;
    end
    if (cfg_write_en) begin
      mem[cfg_write_addr] <= cfg_write_data;
    end
  end
endmodule : scratchpad_bank
