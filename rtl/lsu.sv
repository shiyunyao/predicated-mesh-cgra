// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module lsu #(
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int PRED_WIDTH = cgra_pkg::PRED_WIDTH,
  parameter int DEPTH = cgra_pkg::SCRATCH_BANK_DEPTH,
  parameter int ADDR_WIDTH = cgra_pkg::SCRATCH_ADDR_WIDTH
) (
  input  logic                 clk,
  input  logic                 rst_n,
  input  logic                 has_lsu,

  input  logic                 scratch_cfg_we,
  input  logic [ADDR_WIDTH-1:0] scratch_cfg_addr,
  input  logic [DATA_WIDTH-1:0] scratch_cfg_wdata,

  input  cgra_pkg::lsu_op_e    lsu_op,
  input  cgra_pkg::data_src_e  lsu_addr_src,
  input  cgra_pkg::data_src_e  lsu_store_data_src,
  input  logic                 lsu_commit_pred_enable,
  input  logic                 lsu_commit_pred_invert,
  input  cgra_pkg::pred_src_e  lsu_commit_pred_src,

  input  logic [DATA_WIDTH-1:0] rf_a_data,
  input  logic [DATA_WIDTH-1:0] rf_b_data,
  input  logic [DATA_WIDTH-1:0] north_data_in,
  input  logic                  north_data_valid,
  input  logic [DATA_WIDTH-1:0] south_data_in,
  input  logic                  south_data_valid,
  input  logic [DATA_WIDTH-1:0] east_data_in,
  input  logic                  east_data_valid,
  input  logic [DATA_WIDTH-1:0] west_data_in,
  input  logic                  west_data_valid,
  input  logic [DATA_WIDTH-1:0] const_data,
  input  logic [DATA_WIDTH-1:0] lsu_load_data_src,
  input  logic                  lsu_load_data_valid,

  input  logic [PRED_WIDTH-1:0] rf_a_pred,
  input  logic [PRED_WIDTH-1:0] rf_b_pred,
  input  logic [PRED_WIDTH-1:0] north_pred_in,
  input  logic                  north_pred_valid,
  input  logic [PRED_WIDTH-1:0] south_pred_in,
  input  logic                  south_pred_valid,
  input  logic [PRED_WIDTH-1:0] east_pred_in,
  input  logic                  east_pred_valid,
  input  logic [PRED_WIDTH-1:0] west_pred_in,
  input  logic                  west_pred_valid,

  output logic                  load_resp_valid,
  output logic [DATA_WIDTH-1:0] load_resp_data
);
  logic addr_consume;
  logic store_data_consume;
  logic pred_consume;
  logic [DATA_WIDTH-1:0] addr_data;
  logic addr_valid;
  logic [DATA_WIDTH-1:0] store_data;
  logic store_data_valid;
  logic [PRED_WIDTH-1:0] commit_pred;
  logic commit_pred_valid;
  logic commit_allow;
  logic store_commit;
  logic issue_load;
  logic [DATA_WIDTH-1:0] scratch_read_data;
  logic load_valid_q0;
  logic [DATA_WIDTH-1:0] load_data_q0;

  assign addr_consume = (lsu_op == cgra_pkg::LSU_OP_LOAD) || (lsu_op == cgra_pkg::LSU_OP_STORE);
  assign store_data_consume = (lsu_op == cgra_pkg::LSU_OP_STORE);
  assign pred_consume = (lsu_op == cgra_pkg::LSU_OP_STORE) && lsu_commit_pred_enable;

  data_src_mux #(
    .WIDTH(DATA_WIDTH)
  ) addr_mux (
    .consume(addr_consume),
    .src_sel(lsu_addr_src),
    .rf_a_data(rf_a_data),
    .rf_b_data(rf_b_data),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data(lsu_load_data_src),
    .lsu_load_valid(lsu_load_data_valid),
    .data_out(addr_data),
    .src_valid(addr_valid)
  );

  data_src_mux #(
    .WIDTH(DATA_WIDTH)
  ) store_data_mux (
    .consume(store_data_consume),
    .src_sel(lsu_store_data_src),
    .rf_a_data(rf_a_data),
    .rf_b_data(rf_b_data),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data(lsu_load_data_src),
    .lsu_load_valid(lsu_load_data_valid),
    .data_out(store_data),
    .src_valid(store_data_valid)
  );

  pred_src_mux #(
    .WIDTH(PRED_WIDTH)
  ) commit_pred_mux (
    .consume(pred_consume),
    .src_sel(lsu_commit_pred_src),
    .rf_a_pred(rf_a_pred),
    .rf_b_pred(rf_b_pred),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .pred_out(commit_pred),
    .src_valid(commit_pred_valid)
  );

  assign commit_allow = !lsu_commit_pred_enable
                        || (lsu_commit_pred_invert ? !commit_pred[0] : commit_pred[0]);
  assign store_commit = has_lsu
                        && (lsu_op == cgra_pkg::LSU_OP_STORE)
                        && addr_valid
                        && store_data_valid
                        && (!lsu_commit_pred_enable || commit_pred_valid)
                        && commit_allow;
  assign issue_load = has_lsu && (lsu_op == cgra_pkg::LSU_OP_LOAD) && addr_valid;

  scratchpad_bank #(
    .DATA_WIDTH(DATA_WIDTH),
    .DEPTH(DEPTH),
    .ADDR_WIDTH(ADDR_WIDTH)
  ) bank (
    .clk(clk),
    .write_en(store_commit),
    .write_addr(addr_data[ADDR_WIDTH-1:0]),
    .write_data(store_data),
    .cfg_write_en(scratch_cfg_we && has_lsu),
    .cfg_write_addr(scratch_cfg_addr),
    .cfg_write_data(scratch_cfg_wdata),
    .read_addr(addr_data[ADDR_WIDTH-1:0]),
    .read_data(scratch_read_data)
  );

`ifndef SYNTHESIS
  always_comb begin
    if (!has_lsu && (lsu_op != cgra_pkg::LSU_OP_NONE)) begin
      $fatal(1, "Active LSU operation on non-LSU tile: op=%0d", lsu_op);
    end
    if (lsu_op == cgra_pkg::LSU_OP_RESERVED) begin
      $fatal(1, "Reserved LSU operation issued");
    end
    if ((lsu_op == cgra_pkg::LSU_OP_LOAD) && lsu_commit_pred_enable) begin
      $fatal(1, "LSU load with predicate commit is illegal");
    end
    if (addr_consume && addr_valid && (addr_data >= DATA_WIDTH'(DEPTH))) begin
      $fatal(1, "LSU scratchpad address out of range: addr=%0d depth=%0d", addr_data, DEPTH);
    end
  end
`endif

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      load_resp_valid <= 1'b0;
      load_resp_data <= '0;
      load_valid_q0 <= 1'b0;
      load_data_q0 <= '0;
    end else begin
      load_resp_valid <= load_valid_q0;
      load_resp_data <= load_data_q0;
      load_valid_q0 <= issue_load;
      load_data_q0 <= scratch_read_data;
    end
  end
endmodule : lsu
