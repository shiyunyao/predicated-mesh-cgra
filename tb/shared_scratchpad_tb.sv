// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module shared_scratchpad_tb(input logic clk);
  import cgra_pkg::*;

  logic [SHARED_MEM_PORTS-1:0] req_valid;
  logic [SHARED_MEM_PORTS-1:0] req_write;
  logic [SHARED_MEM_PORTS*SCRATCHPAD_ADDR_WIDTH-1:0] req_addr;
  logic [SHARED_MEM_PORTS*DATA_WIDTH-1:0] req_wdata;
  logic [SHARED_MEM_PORTS*DATA_WIDTH-1:0] read_data;
  logic cfg_write_en;
  logic [SCRATCHPAD_ADDR_WIDTH-1:0] cfg_write_addr;
  logic [DATA_WIDTH-1:0] cfg_write_data;
  int cycle;
  logic conflict_store_load;
  logic conflict_store_store;

  shared_scratchpad dut (
    .clk(clk),
    .req_valid(req_valid),
    .req_write(req_write),
    .req_addr(req_addr),
    .req_wdata(req_wdata),
    .read_data(read_data),
    .cfg_write_en(cfg_write_en),
    .cfg_write_addr(cfg_write_addr),
    .cfg_write_data(cfg_write_data)
  );

  task automatic set_port(input int port,
                          input logic valid,
                          input logic write,
                          input logic [SCRATCHPAD_ADDR_WIDTH-1:0] addr,
                          input logic [DATA_WIDTH-1:0] data);
    begin
      req_valid[port] <= valid;
      req_write[port] <= write;
      req_addr[port*SCRATCHPAD_ADDR_WIDTH +: SCRATCHPAD_ADDR_WIDTH] <= addr;
      req_wdata[port*DATA_WIDTH +: DATA_WIDTH] <= data;
    end
  endtask

  task automatic expect_data(input string name,
                             input logic [DATA_WIDTH-1:0] actual,
                             input logic [DATA_WIDTH-1:0] expected);
    begin
      if (actual !== expected) begin
        $fatal(1, "%s: expected 0x%08h, got 0x%08h", name, expected, actual);
      end
    end
  endtask

  initial begin
    cycle = 0;
    req_valid = '0;
    req_write = '0;
    req_addr = '0;
    req_wdata = '0;
    cfg_write_en = 1'b0;
    cfg_write_addr = '0;
    cfg_write_data = '0;
    conflict_store_load = ($test$plusargs("CONFLICT_STORE_LOAD") != 0);
    conflict_store_store = ($test$plusargs("CONFLICT_STORE_STORE") != 0);
  end

  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    req_valid <= '0;
    req_write <= '0;
    req_addr <= '0;
    req_wdata <= '0;
    cfg_write_en <= 1'b0;

    unique case (cycle)
      0: begin
        cfg_write_en <= 1'b1;
        cfg_write_addr <= SCRATCHPAD_ADDR_WIDTH'(0);
        cfg_write_data <= 32'h1111_0000;
      end
      1: begin
        expect_data("config write address 0", read_data[0 +: DATA_WIDTH], 32'h1111_0000);
        cfg_write_en <= 1'b1;
        cfg_write_addr <= SCRATCHPAD_ADDR_WIDTH'(SCRATCHPAD_DEPTH - 1);
        cfg_write_data <= 32'hffff_0000;
        req_addr[0 +: SCRATCHPAD_ADDR_WIDTH] <= SCRATCHPAD_ADDR_WIDTH'(SCRATCHPAD_DEPTH - 1);
      end
      2: begin
        expect_data("config write highest address", read_data[0 +: DATA_WIDTH], 32'hffff_0000);
        for (int port = 0; port < SHARED_MEM_PORTS; port = port + 1) begin
          set_port(port, 1'b1, 1'b1, SCRATCHPAD_ADDR_WIDTH'(100 + port), 32'h1000_0000 + port);
        end
      end
      3: begin
        for (int port = 0; port < SHARED_MEM_PORTS; port = port + 1) begin
          set_port(port, 1'b1, 1'b0, SCRATCHPAD_ADDR_WIDTH'(100 + port), '0);
        end
      end
      4: begin
        for (int port = 0; port < SHARED_MEM_PORTS; port = port + 1) begin
          expect_data("four-port independent read", read_data[port*DATA_WIDTH +: DATA_WIDTH], 32'h1000_0000 + port);
          set_port(port, 1'b1, 1'b0, SCRATCHPAD_ADDR_WIDTH'(200), '0);
        end
      end
      5: begin
        for (int port = 0; port < SHARED_MEM_PORTS; port = port + 1) begin
          expect_data("same-address multi-load", read_data[port*DATA_WIDTH +: DATA_WIDTH], '0);
        end
        if (conflict_store_load) begin
          set_port(0, 1'b1, 1'b1, SCRATCHPAD_ADDR_WIDTH'(300), 32'h3000_0000);
          set_port(1, 1'b1, 1'b0, SCRATCHPAD_ADDR_WIDTH'(300), '0);
        end else if (conflict_store_store) begin
          set_port(0, 1'b1, 1'b1, SCRATCHPAD_ADDR_WIDTH'(300), 32'h3000_0000);
          set_port(1, 1'b1, 1'b1, SCRATCHPAD_ADDR_WIDTH'(300), 32'h3000_0001);
        end
      end
      6: begin
        if (conflict_store_load || conflict_store_store) begin
          $fatal(1, "shared scratchpad conflict was not rejected");
        end
        $display("shared_scratchpad_tb PASS");
        $finish;
      end
      default: $finish;
    endcase
  end
endmodule : shared_scratchpad_tb
