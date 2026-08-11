// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module mesh #(
  parameter int ROWS = 1,
  parameter int COLS = 2,
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int PRED_WIDTH = cgra_pkg::PRED_WIDTH,
  parameter int TILES = ROWS * COLS,
  parameter int CONTROL_WIDTH = cgra_pkg::CONTROL_WORD_PHYSICAL_WIDTH,
  parameter logic [TILES-1:0] HAS_LSU_MASK = '0
) (
  input  logic                              clk,
  input  logic                              rst_n,
  input  logic [TILES*CONTROL_WIDTH-1:0]    control_words,
  input  logic [TILES-1:0]                  const_cfg_we,
  input  logic [TILES*cgra_pkg::CONST_ADDR_WIDTH-1:0] const_cfg_addr,
  input  logic [TILES*DATA_WIDTH-1:0]       const_cfg_wdata,
  input  logic [TILES-1:0]                  scratch_cfg_we,
  input  logic [TILES*cgra_pkg::SCRATCH_ADDR_WIDTH-1:0] scratch_cfg_addr,
  input  logic [TILES*DATA_WIDTH-1:0]       scratch_cfg_wdata,

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
          .scratch_cfg_we(scratch_cfg_we[IDX]),
          .scratch_cfg_addr(scratch_cfg_addr[IDX*cgra_pkg::SCRATCH_ADDR_WIDTH +: cgra_pkg::SCRATCH_ADDR_WIDTH]),
          .scratch_cfg_wdata(scratch_cfg_wdata[IDX*DATA_WIDTH +: DATA_WIDTH]),
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
