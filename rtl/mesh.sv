// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module mesh #(
  parameter int ROWS = 1,
  parameter int COLS = 2,
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int PRED_WIDTH = cgra_pkg::PRED_WIDTH,
  parameter int TILES = ROWS * COLS,
  parameter int CONTROL_WIDTH = cgra_pkg::CONTROL_WORD_PHYSICAL_WIDTH,
  parameter int SHARED_MEM_PORTS = cgra_pkg::SHARED_MEM_PORTS,
  parameter logic [TILES-1:0] HAS_LSU_MASK = '0
) (
  input  logic                              clk,
  input  logic                              rst_n,
  input  logic [TILES*CONTROL_WIDTH-1:0]    control_words,
  input  logic [TILES-1:0]                  const_cfg_we,
  input  logic [TILES*cgra_pkg::CONST_ADDR_WIDTH-1:0] const_cfg_addr,
  input  logic [TILES*DATA_WIDTH-1:0]       const_cfg_wdata,
  output logic [SHARED_MEM_PORTS-1:0]       mem_req_valid,
  output logic [SHARED_MEM_PORTS-1:0]       mem_req_write,
  output logic [SHARED_MEM_PORTS*cgra_pkg::SCRATCHPAD_ADDR_WIDTH-1:0] mem_req_addr,
  output logic [SHARED_MEM_PORTS*DATA_WIDTH-1:0] mem_req_wdata,
  input  logic [SHARED_MEM_PORTS*DATA_WIDTH-1:0] mem_read_data,

  output logic [TILES-1:0]                  north_data_we,
  output logic [TILES*DATA_WIDTH-1:0]       north_data_out,
  output logic [TILES-1:0]                  south_data_we,
  output logic [TILES*DATA_WIDTH-1:0]       south_data_out,
  output logic [TILES-1:0]                  east_data_we,
  output logic [TILES*DATA_WIDTH-1:0]       east_data_out,
  output logic [TILES-1:0]                  west_data_we,
  output logic [TILES*DATA_WIDTH-1:0]       west_data_out,

  output logic [TILES-1:0]                  north_pred_we,
  output logic [TILES*PRED_WIDTH-1:0]       north_pred_out,
  output logic [TILES-1:0]                  south_pred_we,
  output logic [TILES*PRED_WIDTH-1:0]       south_pred_out,
  output logic [TILES-1:0]                  east_pred_we,
  output logic [TILES*PRED_WIDTH-1:0]       east_pred_out,
  output logic [TILES-1:0]                  west_pred_we,
  output logic [TILES*PRED_WIDTH-1:0]       west_pred_out
);
  logic [DATA_WIDTH-1:0] tile_north_data_in [TILES];
  logic tile_north_data_valid [TILES];
  logic [DATA_WIDTH-1:0] tile_south_data_in [TILES];
  logic tile_south_data_valid [TILES];
  logic [DATA_WIDTH-1:0] tile_east_data_in [TILES];
  logic tile_east_data_valid [TILES];
  logic [DATA_WIDTH-1:0] tile_west_data_in [TILES];
  logic tile_west_data_valid [TILES];

  logic [DATA_WIDTH-1:0] tile_north_data_out [TILES];
  logic tile_north_data_we [TILES];
  logic [DATA_WIDTH-1:0] tile_south_data_out [TILES];
  logic tile_south_data_we [TILES];
  logic [DATA_WIDTH-1:0] tile_east_data_out [TILES];
  logic tile_east_data_we [TILES];
  logic [DATA_WIDTH-1:0] tile_west_data_out [TILES];
  logic tile_west_data_we [TILES];

  logic [PRED_WIDTH-1:0] tile_north_pred_in [TILES];
  logic tile_north_pred_valid [TILES];
  logic [PRED_WIDTH-1:0] tile_south_pred_in [TILES];
  logic tile_south_pred_valid [TILES];
  logic [PRED_WIDTH-1:0] tile_east_pred_in [TILES];
  logic tile_east_pred_valid [TILES];
  logic [PRED_WIDTH-1:0] tile_west_pred_in [TILES];
  logic tile_west_pred_valid [TILES];

  logic [PRED_WIDTH-1:0] tile_north_pred_out [TILES];
  logic tile_north_pred_we [TILES];
  logic [PRED_WIDTH-1:0] tile_south_pred_out [TILES];
  logic tile_south_pred_we [TILES];
  logic [PRED_WIDTH-1:0] tile_east_pred_out [TILES];
  logic tile_east_pred_we [TILES];
  logic [PRED_WIDTH-1:0] tile_west_pred_out [TILES];
  logic tile_west_pred_we [TILES];

  logic tile_mem_req_valid [TILES];
  logic tile_mem_req_write [TILES];
  logic [cgra_pkg::SCRATCHPAD_ADDR_WIDTH-1:0] tile_mem_req_addr [TILES];
  logic [DATA_WIDTH-1:0] tile_mem_req_wdata [TILES];
  logic [DATA_WIDTH-1:0] tile_mem_read_data [TILES];

  function automatic int lsu_port_for_idx(input int idx);
    int rank;
    begin
      rank = 0;
      for (int previous = 0; previous < idx; previous = previous + 1) begin
        rank += int'(HAS_LSU_MASK[previous]);
      end
      lsu_port_for_idx = rank;
    end
  endfunction

`ifndef SYNTHESIS
  initial begin
    if (int'($countones(HAS_LSU_MASK)) > SHARED_MEM_PORTS) begin
      $fatal(1, "HAS_LSU_MASK enables %0d LSUs but only %0d shared memory ports exist",
             $countones(HAS_LSU_MASK), SHARED_MEM_PORTS);
    end
  end
`endif

  always_comb begin
    mem_req_valid = '0;
    mem_req_write = '0;
    mem_req_addr = '0;
    mem_req_wdata = '0;
    for (int idx = 0; idx < TILES; idx = idx + 1) begin
      tile_mem_read_data[idx] = '0;
      if (HAS_LSU_MASK[idx]) begin
        mem_req_valid[lsu_port_for_idx(idx)] = tile_mem_req_valid[idx];
        mem_req_write[lsu_port_for_idx(idx)] = tile_mem_req_write[idx];
        mem_req_addr[lsu_port_for_idx(idx)*cgra_pkg::SCRATCHPAD_ADDR_WIDTH +: cgra_pkg::SCRATCHPAD_ADDR_WIDTH] = tile_mem_req_addr[idx];
        mem_req_wdata[lsu_port_for_idx(idx)*DATA_WIDTH +: DATA_WIDTH] = tile_mem_req_wdata[idx];
        tile_mem_read_data[idx] = mem_read_data[lsu_port_for_idx(idx)*DATA_WIDTH +: DATA_WIDTH];
      end
    end
  end

  genvar row;
  genvar col;
  generate
    for (row = 0; row < ROWS; row = row + 1) begin : row_gen
      for (col = 0; col < COLS; col = col + 1) begin : col_gen
        localparam int IDX = row * COLS + col;
        if (row == 0) begin : north_boundary
          assign tile_north_data_in[IDX] = '0;
          assign tile_north_data_valid[IDX] = 1'b0;
          assign tile_north_pred_in[IDX] = '0;
          assign tile_north_pred_valid[IDX] = 1'b0;
        end else begin : north_link
          localparam int NORTH_IDX = (row - 1) * COLS + col;
          assign tile_north_data_in[IDX] = tile_south_data_out[NORTH_IDX];
          assign tile_north_data_valid[IDX] = tile_south_data_we[NORTH_IDX];
          assign tile_north_pred_in[IDX] = tile_south_pred_out[NORTH_IDX];
          assign tile_north_pred_valid[IDX] = tile_south_pred_we[NORTH_IDX];
        end

        if (row == ROWS - 1) begin : south_boundary
          assign tile_south_data_in[IDX] = '0;
          assign tile_south_data_valid[IDX] = 1'b0;
          assign tile_south_pred_in[IDX] = '0;
          assign tile_south_pred_valid[IDX] = 1'b0;
        end else begin : south_link
          localparam int SOUTH_IDX = (row + 1) * COLS + col;
          assign tile_south_data_in[IDX] = tile_north_data_out[SOUTH_IDX];
          assign tile_south_data_valid[IDX] = tile_north_data_we[SOUTH_IDX];
          assign tile_south_pred_in[IDX] = tile_north_pred_out[SOUTH_IDX];
          assign tile_south_pred_valid[IDX] = tile_north_pred_we[SOUTH_IDX];
        end

        if (col == COLS - 1) begin : east_boundary
          assign tile_east_data_in[IDX] = '0;
          assign tile_east_data_valid[IDX] = 1'b0;
          assign tile_east_pred_in[IDX] = '0;
          assign tile_east_pred_valid[IDX] = 1'b0;
        end else begin : east_link
          localparam int EAST_IDX = row * COLS + col + 1;
          assign tile_east_data_in[IDX] = tile_west_data_out[EAST_IDX];
          assign tile_east_data_valid[IDX] = tile_west_data_we[EAST_IDX];
          assign tile_east_pred_in[IDX] = tile_west_pred_out[EAST_IDX];
          assign tile_east_pred_valid[IDX] = tile_west_pred_we[EAST_IDX];
        end

        if (col == 0) begin : west_boundary
          assign tile_west_data_in[IDX] = '0;
          assign tile_west_data_valid[IDX] = 1'b0;
          assign tile_west_pred_in[IDX] = '0;
          assign tile_west_pred_valid[IDX] = 1'b0;
        end else begin : west_link
          localparam int WEST_IDX = row * COLS + col - 1;
          assign tile_west_data_in[IDX] = tile_east_data_out[WEST_IDX];
          assign tile_west_data_valid[IDX] = tile_east_data_we[WEST_IDX];
          assign tile_west_pred_in[IDX] = tile_east_pred_out[WEST_IDX];
          assign tile_west_pred_valid[IDX] = tile_east_pred_we[WEST_IDX];
        end

        tile #(
          .HAS_LSU(HAS_LSU_MASK[IDX])
        ) tile_i (
          .clk(clk),
          .rst_n(rst_n),
          .control_word(control_words[IDX*CONTROL_WIDTH +: CONTROL_WIDTH]),
          .const_cfg_we(const_cfg_we[IDX]),
          .const_cfg_addr(const_cfg_addr[IDX*cgra_pkg::CONST_ADDR_WIDTH +: cgra_pkg::CONST_ADDR_WIDTH]),
          .const_cfg_wdata(const_cfg_wdata[IDX*DATA_WIDTH +: DATA_WIDTH]),
          .mem_read_data(tile_mem_read_data[IDX]),
          .mem_req_valid(tile_mem_req_valid[IDX]),
          .mem_req_write(tile_mem_req_write[IDX]),
          .mem_req_addr(tile_mem_req_addr[IDX]),
          .mem_req_wdata(tile_mem_req_wdata[IDX]),
          .north_data_in(tile_north_data_in[IDX]),
          .north_data_valid(tile_north_data_valid[IDX]),
          .south_data_in(tile_south_data_in[IDX]),
          .south_data_valid(tile_south_data_valid[IDX]),
          .east_data_in(tile_east_data_in[IDX]),
          .east_data_valid(tile_east_data_valid[IDX]),
          .west_data_in(tile_west_data_in[IDX]),
          .west_data_valid(tile_west_data_valid[IDX]),
          .lsu_load_data('0),
          .lsu_load_valid(1'b0),
          .north_pred_in(tile_north_pred_in[IDX]),
          .north_pred_valid(tile_north_pred_valid[IDX]),
          .south_pred_in(tile_south_pred_in[IDX]),
          .south_pred_valid(tile_south_pred_valid[IDX]),
          .east_pred_in(tile_east_pred_in[IDX]),
          .east_pred_valid(tile_east_pred_valid[IDX]),
          .west_pred_in(tile_west_pred_in[IDX]),
          .west_pred_valid(tile_west_pred_valid[IDX]),
          .north_data_we(tile_north_data_we[IDX]),
          .north_data_out(tile_north_data_out[IDX]),
          .south_data_we(tile_south_data_we[IDX]),
          .south_data_out(tile_south_data_out[IDX]),
          .east_data_we(tile_east_data_we[IDX]),
          .east_data_out(tile_east_data_out[IDX]),
          .west_data_we(tile_west_data_we[IDX]),
          .west_data_out(tile_west_data_out[IDX]),
          .north_pred_we(tile_north_pred_we[IDX]),
          .north_pred_out(tile_north_pred_out[IDX]),
          .south_pred_we(tile_south_pred_we[IDX]),
          .south_pred_out(tile_south_pred_out[IDX]),
          .east_pred_we(tile_east_pred_we[IDX]),
          .east_pred_out(tile_east_pred_out[IDX]),
          .west_pred_we(tile_west_pred_we[IDX]),
          .west_pred_out(tile_west_pred_out[IDX])
        );

        assign north_data_we[IDX] = tile_north_data_we[IDX];
        assign north_data_out[IDX*DATA_WIDTH +: DATA_WIDTH] = tile_north_data_out[IDX];
        assign south_data_we[IDX] = tile_south_data_we[IDX];
        assign south_data_out[IDX*DATA_WIDTH +: DATA_WIDTH] = tile_south_data_out[IDX];
        assign east_data_we[IDX] = tile_east_data_we[IDX];
        assign east_data_out[IDX*DATA_WIDTH +: DATA_WIDTH] = tile_east_data_out[IDX];
        assign west_data_we[IDX] = tile_west_data_we[IDX];
        assign west_data_out[IDX*DATA_WIDTH +: DATA_WIDTH] = tile_west_data_out[IDX];

        assign north_pred_we[IDX] = tile_north_pred_we[IDX];
        assign north_pred_out[IDX*PRED_WIDTH +: PRED_WIDTH] = tile_north_pred_out[IDX];
        assign south_pred_we[IDX] = tile_south_pred_we[IDX];
        assign south_pred_out[IDX*PRED_WIDTH +: PRED_WIDTH] = tile_south_pred_out[IDX];
        assign east_pred_we[IDX] = tile_east_pred_we[IDX];
        assign east_pred_out[IDX*PRED_WIDTH +: PRED_WIDTH] = tile_east_pred_out[IDX];
        assign west_pred_we[IDX] = tile_west_pred_we[IDX];
        assign west_pred_out[IDX*PRED_WIDTH +: PRED_WIDTH] = tile_west_pred_out[IDX];
      end
    end
  endgenerate
endmodule : mesh
