// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module tile #(
  parameter int DATA_WIDTH = cgra_pkg::DATA_WIDTH,
  parameter int PRED_WIDTH = cgra_pkg::PRED_WIDTH,
  parameter bit HAS_LSU = 1'b0
) (
  input  logic                         clk,
  input  logic                         rst_n,
  input  cgra_pkg::control_word_bits_t control_word,

  input  logic                         const_cfg_we,
  input  logic [cgra_pkg::CONST_ADDR_WIDTH-1:0] const_cfg_addr,
  input  logic [DATA_WIDTH-1:0]        const_cfg_wdata,
  input  logic                         scratch_cfg_we,
  input  logic [cgra_pkg::SCRATCH_ADDR_WIDTH-1:0] scratch_cfg_addr,
  input  logic [DATA_WIDTH-1:0]        scratch_cfg_wdata,

  input  logic [DATA_WIDTH-1:0]        north_data_in,
  input  logic                         north_data_valid,
  input  logic [DATA_WIDTH-1:0]        south_data_in,
  input  logic                         south_data_valid,
  input  logic [DATA_WIDTH-1:0]        east_data_in,
  input  logic                         east_data_valid,
  input  logic [DATA_WIDTH-1:0]        west_data_in,
  input  logic                         west_data_valid,
  input  logic [DATA_WIDTH-1:0]        lsu_load_data,
  input  logic                         lsu_load_valid,

  input  logic [PRED_WIDTH-1:0]        north_pred_in,
  input  logic                         north_pred_valid,
  input  logic [PRED_WIDTH-1:0]        south_pred_in,
  input  logic                         south_pred_valid,
  input  logic [PRED_WIDTH-1:0]        east_pred_in,
  input  logic                         east_pred_valid,
  input  logic [PRED_WIDTH-1:0]        west_pred_in,
  input  logic                         west_pred_valid,

  output logic                         north_data_we,
  output logic [DATA_WIDTH-1:0]        north_data_out,
  output logic                         south_data_we,
  output logic [DATA_WIDTH-1:0]        south_data_out,
  output logic                         east_data_we,
  output logic [DATA_WIDTH-1:0]        east_data_out,
  output logic                         west_data_we,
  output logic [DATA_WIDTH-1:0]        west_data_out,

  output logic                         north_pred_we,
  output logic [PRED_WIDTH-1:0]        north_pred_out,
  output logic                         south_pred_we,
  output logic [PRED_WIDTH-1:0]        south_pred_out,
  output logic                         east_pred_we,
  output logic [PRED_WIDTH-1:0]        east_pred_out,
  output logic                         west_pred_we,
  output logic [PRED_WIDTH-1:0]        west_pred_out
);
  cgra_pkg::tile_control_word_t ctrl;
  logic [DATA_WIDTH-1:0] const_mem [cgra_pkg::CONST_MEM_DEPTH];
  logic [DATA_WIDTH-1:0] const_data;

  logic data_rf_ren_a;
  logic data_rf_ren_b;
  logic [DATA_WIDTH-1:0] data_rf_rdata_a;
  logic [DATA_WIDTH-1:0] data_rf_rdata_b;
  logic [DATA_WIDTH-1:0] data_src_a;
  logic [DATA_WIDTH-1:0] data_src_b;
  logic [DATA_WIDTH-1:0] data_w1_data;
  logic data_src_a_valid_unused;
  logic data_src_b_valid_unused;
  logic data_w1_valid_unused;

  logic pred_rf_ren_a;
  logic pred_rf_ren_b;
  logic [PRED_WIDTH-1:0] pred_rf_rdata_a;
  logic [PRED_WIDTH-1:0] pred_rf_rdata_b;
  logic [PRED_WIDTH-1:0] pred_src_p0;
  logic [PRED_WIDTH-1:0] pred_src_p1;
  logic [PRED_WIDTH-1:0] pred_w1_data;
  logic pred_src_p0_valid_unused;
  logic pred_src_p1_valid_unused;
  logic pred_w1_valid_unused;

  logic [DATA_WIDTH-1:0] fu_data_result;
  logic fu_data_result_valid;
  logic [PRED_WIDTH-1:0] fu_pred_result;
  logic fu_pred_result_valid;

  logic [DATA_WIDTH-1:0] lsu_resp_data;
  logic lsu_resp_valid;
  logic [DATA_WIDTH-1:0] tile_lsu_load_data;
  logic tile_lsu_load_valid;

  logic route_north_data_we;
  logic [DATA_WIDTH-1:0] route_north_data_out;
  logic route_south_data_we;
  logic [DATA_WIDTH-1:0] route_south_data_out;
  logic route_east_data_we;
  logic [DATA_WIDTH-1:0] route_east_data_out;
  logic route_west_data_we;
  logic [DATA_WIDTH-1:0] route_west_data_out;
  logic route_north_pred_we;
  logic [PRED_WIDTH-1:0] route_north_pred_out;
  logic route_south_pred_we;
  logic [PRED_WIDTH-1:0] route_south_pred_out;
  logic route_east_pred_we;
  logic [PRED_WIDTH-1:0] route_east_pred_out;
  logic route_west_pred_we;
  logic [PRED_WIDTH-1:0] route_west_pred_out;

  logic op_uses_data_a;
  logic op_uses_data_b;
  logic op_uses_pred_p0;
  logic op_uses_pred_p1;
  logic unused_valids;

  assign ctrl = cgra_pkg::unpack_tile_control_word(control_word);
  assign tile_lsu_load_data = HAS_LSU ? lsu_resp_data : lsu_load_data;
  assign tile_lsu_load_valid = HAS_LSU ? lsu_resp_valid : lsu_load_valid;
  assign unused_valids = data_src_a_valid_unused
                         ^ data_src_b_valid_unused
                         ^ data_w1_valid_unused
                         ^ pred_src_p0_valid_unused
                         ^ pred_src_p1_valid_unused
                         ^ pred_w1_valid_unused;
  assign const_data = const_mem[ctrl.const_ctrl.const_addr];

  always_comb begin
    op_uses_data_a = 1'b0;
    op_uses_data_b = 1'b0;
    op_uses_pred_p0 = 1'b0;
    op_uses_pred_p1 = 1'b0;

    unique case (ctrl.exec_ctrl.op)
      cgra_pkg::OP_PASS: begin
        op_uses_data_a = 1'b1;
      end
      cgra_pkg::OP_ADD,
      cgra_pkg::OP_SUB,
      cgra_pkg::OP_MUL,
      cgra_pkg::OP_AND,
      cgra_pkg::OP_OR,
      cgra_pkg::OP_XOR,
      cgra_pkg::OP_SHL,
      cgra_pkg::OP_LSHR,
      cgra_pkg::OP_CMP_EQ,
      cgra_pkg::OP_CMP_NE,
      cgra_pkg::OP_CMP_ULT,
      cgra_pkg::OP_CMP_ULE: begin
        op_uses_data_a = 1'b1;
        op_uses_data_b = 1'b1;
      end
      cgra_pkg::OP_SELECT: begin
        op_uses_data_a = 1'b1;
        op_uses_data_b = 1'b1;
        op_uses_pred_p0 = 1'b1;
      end
      cgra_pkg::OP_PPASS,
      cgra_pkg::OP_PNOT: begin
        op_uses_pred_p0 = 1'b1;
      end
      cgra_pkg::OP_PAND,
      cgra_pkg::OP_POR: begin
        op_uses_pred_p0 = 1'b1;
        op_uses_pred_p1 = 1'b1;
      end
      default: begin
      end
    endcase
  end

  always_comb begin
    data_rf_ren_a = ((op_uses_data_a && (ctrl.exec_ctrl.src_a_sel == cgra_pkg::DATA_SRC_RF_A))
                     || (op_uses_data_b && (ctrl.exec_ctrl.src_b_sel == cgra_pkg::DATA_SRC_RF_A))
                     || (ctrl.data_rf_ctrl.data_w1_we && (ctrl.data_rf_ctrl.data_w1_src == cgra_pkg::DATA_SRC_RF_A))
                     || (ctrl.data_route_ctrl.north.we && (ctrl.data_route_ctrl.north.src == cgra_pkg::DATA_ROUTE_SRC_RF_A))
                     || (ctrl.data_route_ctrl.south.we && (ctrl.data_route_ctrl.south.src == cgra_pkg::DATA_ROUTE_SRC_RF_A))
                     || (ctrl.data_route_ctrl.east.we && (ctrl.data_route_ctrl.east.src == cgra_pkg::DATA_ROUTE_SRC_RF_A))
                     || (ctrl.data_route_ctrl.west.we && (ctrl.data_route_ctrl.west.src == cgra_pkg::DATA_ROUTE_SRC_RF_A))
                     || (((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_LOAD)
                          || (ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE))
                         && (ctrl.lsu_ctrl.lsu_addr_src == cgra_pkg::DATA_SRC_RF_A))
                     || ((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE)
                         && (ctrl.lsu_ctrl.lsu_store_data_src == cgra_pkg::DATA_SRC_RF_A)));

    data_rf_ren_b = ((op_uses_data_a && (ctrl.exec_ctrl.src_a_sel == cgra_pkg::DATA_SRC_RF_B))
                     || (op_uses_data_b && (ctrl.exec_ctrl.src_b_sel == cgra_pkg::DATA_SRC_RF_B))
                     || (ctrl.data_rf_ctrl.data_w1_we && (ctrl.data_rf_ctrl.data_w1_src == cgra_pkg::DATA_SRC_RF_B))
                     || (ctrl.data_route_ctrl.north.we && (ctrl.data_route_ctrl.north.src == cgra_pkg::DATA_ROUTE_SRC_RF_B))
                     || (ctrl.data_route_ctrl.south.we && (ctrl.data_route_ctrl.south.src == cgra_pkg::DATA_ROUTE_SRC_RF_B))
                     || (ctrl.data_route_ctrl.east.we && (ctrl.data_route_ctrl.east.src == cgra_pkg::DATA_ROUTE_SRC_RF_B))
                     || (ctrl.data_route_ctrl.west.we && (ctrl.data_route_ctrl.west.src == cgra_pkg::DATA_ROUTE_SRC_RF_B))
                     || (((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_LOAD)
                          || (ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE))
                         && (ctrl.lsu_ctrl.lsu_addr_src == cgra_pkg::DATA_SRC_RF_B))
                     || ((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE)
                         && (ctrl.lsu_ctrl.lsu_store_data_src == cgra_pkg::DATA_SRC_RF_B)));

    pred_rf_ren_a = ((op_uses_pred_p0 && (ctrl.exec_ctrl.src_p0_sel == cgra_pkg::PRED_SRC_RF_A))
                     || (op_uses_pred_p1 && (ctrl.exec_ctrl.src_p1_sel == cgra_pkg::PRED_SRC_RF_A))
                     || (ctrl.pred_rf_ctrl.pred_w1_we && (ctrl.pred_rf_ctrl.pred_w1_src == cgra_pkg::PRED_SRC_RF_A))
                     || (ctrl.pred_route_ctrl.north.we && (ctrl.pred_route_ctrl.north.src == cgra_pkg::PRED_ROUTE_SRC_RF_A))
                     || (ctrl.pred_route_ctrl.south.we && (ctrl.pred_route_ctrl.south.src == cgra_pkg::PRED_ROUTE_SRC_RF_A))
                     || (ctrl.pred_route_ctrl.east.we && (ctrl.pred_route_ctrl.east.src == cgra_pkg::PRED_ROUTE_SRC_RF_A))
                     || (ctrl.pred_route_ctrl.west.we && (ctrl.pred_route_ctrl.west.src == cgra_pkg::PRED_ROUTE_SRC_RF_A))
                     || ((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE)
                         && ctrl.lsu_ctrl.lsu_commit_pred_enable
                         && (ctrl.lsu_ctrl.lsu_commit_pred_src == cgra_pkg::PRED_SRC_RF_A)));

    pred_rf_ren_b = ((op_uses_pred_p0 && (ctrl.exec_ctrl.src_p0_sel == cgra_pkg::PRED_SRC_RF_B))
                     || (op_uses_pred_p1 && (ctrl.exec_ctrl.src_p1_sel == cgra_pkg::PRED_SRC_RF_B))
                     || (ctrl.pred_rf_ctrl.pred_w1_we && (ctrl.pred_rf_ctrl.pred_w1_src == cgra_pkg::PRED_SRC_RF_B))
                     || (ctrl.pred_route_ctrl.north.we && (ctrl.pred_route_ctrl.north.src == cgra_pkg::PRED_ROUTE_SRC_RF_B))
                     || (ctrl.pred_route_ctrl.south.we && (ctrl.pred_route_ctrl.south.src == cgra_pkg::PRED_ROUTE_SRC_RF_B))
                     || (ctrl.pred_route_ctrl.east.we && (ctrl.pred_route_ctrl.east.src == cgra_pkg::PRED_ROUTE_SRC_RF_B))
                     || (ctrl.pred_route_ctrl.west.we && (ctrl.pred_route_ctrl.west.src == cgra_pkg::PRED_ROUTE_SRC_RF_B))
                     || ((ctrl.lsu_ctrl.lsu_op == cgra_pkg::LSU_OP_STORE)
                         && ctrl.lsu_ctrl.lsu_commit_pred_enable
                         && (ctrl.lsu_ctrl.lsu_commit_pred_src == cgra_pkg::PRED_SRC_RF_B)));
  end

  data_rf data_rf_i (
    .clk(clk),
    .rst_n(rst_n),
    .ren_a(data_rf_ren_a),
    .raddr_a(ctrl.exec_ctrl.data_rf_raddr_a),
    .rdata_a(data_rf_rdata_a),
    .ren_b(data_rf_ren_b),
    .raddr_b(ctrl.exec_ctrl.data_rf_raddr_b),
    .rdata_b(data_rf_rdata_b),
    .w0_we(ctrl.data_rf_ctrl.data_w0_we),
    .w0_addr(ctrl.data_rf_ctrl.data_w0_addr),
    .w0_data(fu_data_result),
    .w1_we(ctrl.data_rf_ctrl.data_w1_we),
    .w1_addr(ctrl.data_rf_ctrl.data_w1_addr),
    .w1_data(data_w1_data)
  );

  pred_rf pred_rf_i (
    .clk(clk),
    .rst_n(rst_n),
    .ren_a(pred_rf_ren_a),
    .raddr_a(ctrl.exec_ctrl.pred_rf_raddr_a),
    .rdata_a(pred_rf_rdata_a),
    .ren_b(pred_rf_ren_b),
    .raddr_b(ctrl.exec_ctrl.pred_rf_raddr_b),
    .rdata_b(pred_rf_rdata_b),
    .pw0_we(ctrl.pred_rf_ctrl.pred_w0_we),
    .pw0_addr(ctrl.pred_rf_ctrl.pred_w0_addr),
    .pw0_data(fu_pred_result),
    .pw1_we(ctrl.pred_rf_ctrl.pred_w1_we),
    .pw1_addr(ctrl.pred_rf_ctrl.pred_w1_addr),
    .pw1_data(pred_w1_data)
  );

  data_src_mux src_a_mux (
    .consume(op_uses_data_a),
    .src_sel(ctrl.exec_ctrl.src_a_sel),
    .rf_a_data(data_rf_rdata_a),
    .rf_b_data(data_rf_rdata_b),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data(tile_lsu_load_data),
    .lsu_load_valid(tile_lsu_load_valid),
    .data_out(data_src_a),
    .src_valid(data_src_a_valid_unused)
  );

  data_src_mux src_b_mux (
    .consume(op_uses_data_b),
    .src_sel(ctrl.exec_ctrl.src_b_sel),
    .rf_a_data(data_rf_rdata_a),
    .rf_b_data(data_rf_rdata_b),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data(tile_lsu_load_data),
    .lsu_load_valid(tile_lsu_load_valid),
    .data_out(data_src_b),
    .src_valid(data_src_b_valid_unused)
  );

  data_src_mux data_w1_mux (
    .consume(ctrl.data_rf_ctrl.data_w1_we),
    .src_sel(ctrl.data_rf_ctrl.data_w1_src),
    .rf_a_data(data_rf_rdata_a),
    .rf_b_data(data_rf_rdata_b),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data(tile_lsu_load_data),
    .lsu_load_valid(tile_lsu_load_valid),
    .data_out(data_w1_data),
    .src_valid(data_w1_valid_unused)
  );

  pred_src_mux src_p0_mux (
    .consume(op_uses_pred_p0),
    .src_sel(ctrl.exec_ctrl.src_p0_sel),
    .rf_a_pred(pred_rf_rdata_a),
    .rf_b_pred(pred_rf_rdata_b),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .pred_out(pred_src_p0),
    .src_valid(pred_src_p0_valid_unused)
  );

  pred_src_mux src_p1_mux (
    .consume(op_uses_pred_p1),
    .src_sel(ctrl.exec_ctrl.src_p1_sel),
    .rf_a_pred(pred_rf_rdata_a),
    .rf_b_pred(pred_rf_rdata_b),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .pred_out(pred_src_p1),
    .src_valid(pred_src_p1_valid_unused)
  );

  pred_src_mux pred_w1_mux (
    .consume(ctrl.pred_rf_ctrl.pred_w1_we),
    .src_sel(ctrl.pred_rf_ctrl.pred_w1_src),
    .rf_a_pred(pred_rf_rdata_a),
    .rf_b_pred(pred_rf_rdata_b),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .pred_out(pred_w1_data),
    .src_valid(pred_w1_valid_unused)
  );

  fu fu_i (
    .op(ctrl.exec_ctrl.op),
    .data_a(data_src_a),
    .data_b(data_src_b),
    .pred_p0(pred_src_p0),
    .pred_p1(pred_src_p1),
    .data_result(fu_data_result),
    .data_result_valid(fu_data_result_valid),
    .pred_result(fu_pred_result),
    .pred_result_valid(fu_pred_result_valid)
  );

  lsu #(
    .DATA_WIDTH(DATA_WIDTH),
    .PRED_WIDTH(PRED_WIDTH)
  ) lsu_i (
    .clk(clk),
    .rst_n(rst_n),
    .has_lsu(HAS_LSU),
    .scratch_cfg_we(scratch_cfg_we),
    .scratch_cfg_addr(scratch_cfg_addr),
    .scratch_cfg_wdata(scratch_cfg_wdata),
    .lsu_op(ctrl.lsu_ctrl.lsu_op),
    .lsu_addr_src(ctrl.lsu_ctrl.lsu_addr_src),
    .lsu_store_data_src(ctrl.lsu_ctrl.lsu_store_data_src),
    .lsu_commit_pred_enable(ctrl.lsu_ctrl.lsu_commit_pred_enable),
    .lsu_commit_pred_invert(ctrl.lsu_ctrl.lsu_commit_pred_invert),
    .lsu_commit_pred_src(ctrl.lsu_ctrl.lsu_commit_pred_src),
    .rf_a_data(data_rf_rdata_a),
    .rf_b_data(data_rf_rdata_b),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .const_data(const_data),
    .lsu_load_data_src(tile_lsu_load_data),
    .lsu_load_data_valid(tile_lsu_load_valid),
    .rf_a_pred(pred_rf_rdata_a),
    .rf_b_pred(pred_rf_rdata_b),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .load_resp_valid(lsu_resp_valid),
    .load_resp_data(lsu_resp_data)
  );

  data_switchbox data_switchbox_i (
    .north_we(ctrl.data_route_ctrl.north.we),
    .north_src(ctrl.data_route_ctrl.north.src),
    .south_we(ctrl.data_route_ctrl.south.we),
    .south_src(ctrl.data_route_ctrl.south.src),
    .east_we(ctrl.data_route_ctrl.east.we),
    .east_src(ctrl.data_route_ctrl.east.src),
    .west_we(ctrl.data_route_ctrl.west.we),
    .west_src(ctrl.data_route_ctrl.west.src),
    .north_data_in(north_data_in),
    .north_data_valid(north_data_valid),
    .south_data_in(south_data_in),
    .south_data_valid(south_data_valid),
    .east_data_in(east_data_in),
    .east_data_valid(east_data_valid),
    .west_data_in(west_data_in),
    .west_data_valid(west_data_valid),
    .fu_data(fu_data_result),
    .fu_data_valid(fu_data_result_valid),
    .rf_a_data(data_rf_rdata_a),
    .rf_b_data(data_rf_rdata_b),
    .const_data(const_data),
    .lsu_load_data(tile_lsu_load_data),
    .lsu_load_valid(tile_lsu_load_valid),
    .north_data_we(route_north_data_we),
    .north_data_out(route_north_data_out),
    .south_data_we(route_south_data_we),
    .south_data_out(route_south_data_out),
    .east_data_we(route_east_data_we),
    .east_data_out(route_east_data_out),
    .west_data_we(route_west_data_we),
    .west_data_out(route_west_data_out)
  );

  pred_switchbox pred_switchbox_i (
    .north_we(ctrl.pred_route_ctrl.north.we),
    .north_src(ctrl.pred_route_ctrl.north.src),
    .south_we(ctrl.pred_route_ctrl.south.we),
    .south_src(ctrl.pred_route_ctrl.south.src),
    .east_we(ctrl.pred_route_ctrl.east.we),
    .east_src(ctrl.pred_route_ctrl.east.src),
    .west_we(ctrl.pred_route_ctrl.west.we),
    .west_src(ctrl.pred_route_ctrl.west.src),
    .north_pred_in(north_pred_in),
    .north_pred_valid(north_pred_valid),
    .south_pred_in(south_pred_in),
    .south_pred_valid(south_pred_valid),
    .east_pred_in(east_pred_in),
    .east_pred_valid(east_pred_valid),
    .west_pred_in(west_pred_in),
    .west_pred_valid(west_pred_valid),
    .fu_pred(fu_pred_result),
    .fu_pred_valid(fu_pred_result_valid),
    .rf_a_pred(pred_rf_rdata_a),
    .rf_b_pred(pred_rf_rdata_b),
    .north_pred_we(route_north_pred_we),
    .north_pred_out(route_north_pred_out),
    .south_pred_we(route_south_pred_we),
    .south_pred_out(route_south_pred_out),
    .east_pred_we(route_east_pred_we),
    .east_pred_out(route_east_pred_out),
    .west_pred_we(route_west_pred_we),
    .west_pred_out(route_west_pred_out)
  );

`ifndef SYNTHESIS
  always_comb begin
    if (unused_valids) begin
    end
    if (!HAS_LSU && (ctrl.lsu_ctrl.lsu_op != cgra_pkg::LSU_OP_NONE) && rst_n) begin
      $fatal(1, "Active LSU operation on non-LSU tile: op=%0d", ctrl.lsu_ctrl.lsu_op);
    end
    if (ctrl.data_rf_ctrl.data_w0_we && !fu_data_result_valid && rst_n) begin
      $fatal(1, "DataRF W0 enabled without FU data result: op=%0d", ctrl.exec_ctrl.op);
    end
    if (ctrl.pred_rf_ctrl.pred_w0_we && !fu_pred_result_valid && rst_n) begin
      $fatal(1, "PredicateRF W0 enabled without FU predicate result: op=%0d", ctrl.exec_ctrl.op);
    end
  end
`endif

  always_ff @(posedge clk) begin
    if (const_cfg_we) begin
      const_mem[const_cfg_addr] <= const_cfg_wdata;
    end

    if (!rst_n) begin
      north_data_we <= 1'b0;
      north_data_out <= '0;
      south_data_we <= 1'b0;
      south_data_out <= '0;
      east_data_we <= 1'b0;
      east_data_out <= '0;
      west_data_we <= 1'b0;
      west_data_out <= '0;
      north_pred_we <= 1'b0;
      north_pred_out <= '0;
      south_pred_we <= 1'b0;
      south_pred_out <= '0;
      east_pred_we <= 1'b0;
      east_pred_out <= '0;
      west_pred_we <= 1'b0;
      west_pred_out <= '0;
    end else begin
      north_data_we <= route_north_data_we;
      north_data_out <= route_north_data_out;
      south_data_we <= route_south_data_we;
      south_data_out <= route_south_data_out;
      east_data_we <= route_east_data_we;
      east_data_out <= route_east_data_out;
      west_data_we <= route_west_data_we;
      west_data_out <= route_west_data_out;
      north_pred_we <= route_north_pred_we;
      north_pred_out <= route_north_pred_out;
      south_pred_we <= route_south_pred_we;
      south_pred_out <= route_south_pred_out;
      east_pred_we <= route_east_pred_we;
      east_pred_out <= route_east_pred_out;
      west_pred_we <= route_west_pred_we;
      west_pred_out <= route_west_pred_out;
    end
  end
endmodule : tile
