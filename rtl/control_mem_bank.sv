// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module control_mem_bank #(
  parameter int WIDTH = cgra_pkg::CONTROL_WORD_PHYSICAL_WIDTH,
  parameter int DEPTH = cgra_pkg::CTRL_MEM_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::CTRL_PC_WIDTH,
  parameter int WORD_IDX_WIDTH = cgra_pkg::log2ceil(cgra_pkg::CONTROL_WORD_CHUNKS)
) (
  input  logic                  clk,
  input  logic                  cfg_write_en,
  input  logic [ADDR_WIDTH-1:0] cfg_write_addr,
  input  logic [WORD_IDX_WIDTH-1:0] cfg_write_word_idx,
  input  logic [31:0]           cfg_write_data,
  input  logic [ADDR_WIDTH-1:0] read_addr,
  output logic [WIDTH-1:0]      read_data
);
  logic [WIDTH-1:0] mem [DEPTH];

  assign read_data = mem[read_addr];

  always_ff @(posedge clk) begin
    if (cfg_write_en) begin
      mem[cfg_write_addr][cfg_write_word_idx*32 +: 32] <= cfg_write_data;
    end
  end
endmodule : control_mem_bank
