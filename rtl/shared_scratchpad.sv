// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module shared_scratchpad #(
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int DEPTH = cgra_pkg::SCRATCHPAD_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::SCRATCHPAD_ADDR_WIDTH,
  parameter int PORTS = cgra_pkg::SHARED_MEM_PORTS
) (
  input  logic                         clk,
  input  logic [PORTS-1:0]              req_valid,
  input  logic [PORTS-1:0]              req_write,
  input  logic [PORTS*ADDR_WIDTH-1:0]  req_addr,
  input  logic [PORTS*DATA_WIDTH-1:0]  req_wdata,
  output logic [PORTS*DATA_WIDTH-1:0]  read_data,
  input  logic                         cfg_write_en,
  input  logic [ADDR_WIDTH-1:0]        cfg_write_addr,
  input  logic [DATA_WIDTH-1:0]        cfg_write_data
);
  logic [DATA_WIDTH-1:0] mem [DEPTH];

  genvar port;
  generate
    for (port = 0; port < PORTS; port = port + 1) begin : read_port_gen
      assign read_data[port*DATA_WIDTH +: DATA_WIDTH] =
        mem[req_addr[port*ADDR_WIDTH +: ADDR_WIDTH]];
    end
  endgenerate

`ifndef SYNTHESIS
  generate
    if (DEPTH < (1 << ADDR_WIDTH)) begin : range_assert_gen
      always_comb begin
        if (cfg_write_en && (int'($unsigned(cfg_write_addr)) >= DEPTH)) begin
          $fatal(1, "Shared scratchpad configuration address out of range: addr=%0d depth=%0d",
                 cfg_write_addr, DEPTH);
        end
        for (int i = 0; i < PORTS; i = i + 1) begin
          if (req_valid[i]
              && (int'($unsigned(req_addr[i*ADDR_WIDTH +: ADDR_WIDTH])) >= DEPTH)) begin
            $fatal(1, "Shared scratchpad runtime address out of range: port=%0d addr=%0d depth=%0d",
                   i, req_addr[i*ADDR_WIDTH +: ADDR_WIDTH], DEPTH);
          end
        end
      end
    end
  endgenerate

  always_comb begin
    if (cfg_write_en && (|req_valid)) begin
      $fatal(1, "Shared scratchpad configuration overlaps runtime access");
    end
    for (int i = 0; i < PORTS; i = i + 1) begin
      for (int j = i + 1; j < PORTS; j = j + 1) begin
        if (req_valid[i] && req_valid[j]
            && (req_addr[i*ADDR_WIDTH +: ADDR_WIDTH] == req_addr[j*ADDR_WIDTH +: ADDR_WIDTH])
            && (req_write[i] || req_write[j])) begin
          $fatal(1,
                 "Shared scratchpad same-address conflict: ports=%0d,%0d addr=%0d policy=multi_load_only",
                 i, j, req_addr[i*ADDR_WIDTH +: ADDR_WIDTH]);
        end
      end
    end
  end
`endif

  always_ff @(posedge clk) begin
    for (int i = 0; i < PORTS; i = i + 1) begin
      if (req_valid[i] && req_write[i]) begin
        mem[req_addr[i*ADDR_WIDTH +: ADDR_WIDTH]] <= req_wdata[i*DATA_WIDTH +: DATA_WIDTH];
      end
    end
    if (cfg_write_en) begin
      mem[cfg_write_addr] <= cfg_write_data;
    end
  end
endmodule : shared_scratchpad
