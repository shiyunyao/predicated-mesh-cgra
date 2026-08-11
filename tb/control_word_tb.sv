// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module control_word_tb;
  import cgra_pkg::*;

  task automatic expect_int(input string name, input int actual, input int expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected %0d, got %0d", name, expected, actual);
        $finish;
      end
    end
  endtask

  task automatic expect_bits(input string name,
                             input control_word_bits_t actual,
                             input control_word_bits_t expected);
    begin
      if (actual !== expected) begin
        $error("%s: expected 0x%032h, got 0x%032h", name, expected, actual);
        $finish;
      end
    end
  endtask

`define EXPECT_FIELD(name, actual, expected) expect_int(name, int'(actual), int'(expected))

  tile_control_word_t ctrl;
  tile_control_word_t unpacked;
  control_word_bits_t bits;
  control_word_bits_t expected_bits;

  initial begin
    ctrl = '0;
    ctrl.exec_ctrl.op = OP_SELECT;
    ctrl.exec_ctrl.src_a_sel = DATA_SRC_CONST_DATA;
    ctrl.exec_ctrl.src_b_sel = DATA_SRC_RF_B;
    ctrl.exec_ctrl.src_p0_sel = PRED_SRC_CONST_TRUE;
    ctrl.exec_ctrl.src_p1_sel = PRED_SRC_RF_A;
    ctrl.exec_ctrl.data_rf_raddr_a = 4'h3;
    ctrl.exec_ctrl.data_rf_raddr_b = 4'h4;
    ctrl.exec_ctrl.pred_rf_raddr_a = 4'h5;
    ctrl.exec_ctrl.pred_rf_raddr_b = 4'h6;

    ctrl.data_rf_ctrl.data_w0_we = 1'b1;
    ctrl.data_rf_ctrl.data_w0_addr = 4'h7;
    ctrl.data_rf_ctrl.data_w1_we = 1'b1;
    ctrl.data_rf_ctrl.data_w1_addr = 4'h8;
    ctrl.data_rf_ctrl.data_w1_src = DATA_SRC_LSU_LOAD_DATA;

    ctrl.pred_rf_ctrl.pred_w0_we = 1'b1;
    ctrl.pred_rf_ctrl.pred_w0_addr = 4'h9;
    ctrl.pred_rf_ctrl.pred_w1_we = 1'b0;
    ctrl.pred_rf_ctrl.pred_w1_addr = 4'ha;
    ctrl.pred_rf_ctrl.pred_w1_src = PRED_SRC_CONST_FALSE;

    ctrl.data_route_ctrl.north.we = 1'b1;
    ctrl.data_route_ctrl.north.src = DATA_ROUTE_SRC_FU_DATA;
    ctrl.data_route_ctrl.south.we = 1'b0;
    ctrl.data_route_ctrl.south.src = DATA_ROUTE_SRC_NONE;
    ctrl.data_route_ctrl.east.we = 1'b1;
    ctrl.data_route_ctrl.east.src = DATA_ROUTE_SRC_RF_A;
    ctrl.data_route_ctrl.west.we = 1'b1;
    ctrl.data_route_ctrl.west.src = DATA_ROUTE_SRC_ZERO;

    ctrl.pred_route_ctrl.north.we = 1'b1;
    ctrl.pred_route_ctrl.north.src = PRED_ROUTE_SRC_FU_PRED;
    ctrl.pred_route_ctrl.south.we = 1'b1;
    ctrl.pred_route_ctrl.south.src = PRED_ROUTE_SRC_RF_B;
    ctrl.pred_route_ctrl.east.we = 1'b0;
    ctrl.pred_route_ctrl.east.src = PRED_ROUTE_SRC_NONE;
    ctrl.pred_route_ctrl.west.we = 1'b1;
    ctrl.pred_route_ctrl.west.src = PRED_ROUTE_SRC_CONST_FALSE;

    ctrl.const_ctrl.const_addr = 4'hb;

    ctrl.lsu_ctrl.lsu_op = LSU_OP_STORE;
    ctrl.lsu_ctrl.lsu_addr_src = DATA_SRC_RF_A;
    ctrl.lsu_ctrl.lsu_store_data_src = DATA_SRC_RF_B;
    ctrl.lsu_ctrl.lsu_commit_pred_enable = 1'b1;
    ctrl.lsu_ctrl.lsu_commit_pred_invert = 1'b0;
    ctrl.lsu_ctrl.lsu_commit_pred_src = PRED_SRC_CONST_TRUE;

    bits = pack_tile_control_word(ctrl);
    unpacked = unpack_tile_control_word(bits);

    `EXPECT_FIELD("roundtrip op", int'(unpacked.exec_ctrl.op), int'(ctrl.exec_ctrl.op));
    `EXPECT_FIELD("roundtrip src_a", int'(unpacked.exec_ctrl.src_a_sel), int'(ctrl.exec_ctrl.src_a_sel));
    `EXPECT_FIELD("roundtrip src_b", int'(unpacked.exec_ctrl.src_b_sel), int'(ctrl.exec_ctrl.src_b_sel));
    `EXPECT_FIELD("roundtrip src_p0", int'(unpacked.exec_ctrl.src_p0_sel), int'(ctrl.exec_ctrl.src_p0_sel));
    `EXPECT_FIELD("roundtrip src_p1", int'(unpacked.exec_ctrl.src_p1_sel), int'(ctrl.exec_ctrl.src_p1_sel));
    `EXPECT_FIELD("roundtrip data_rf_raddr_a", unpacked.exec_ctrl.data_rf_raddr_a, ctrl.exec_ctrl.data_rf_raddr_a);
    `EXPECT_FIELD("roundtrip data_rf_raddr_b", unpacked.exec_ctrl.data_rf_raddr_b, ctrl.exec_ctrl.data_rf_raddr_b);
    `EXPECT_FIELD("roundtrip pred_rf_raddr_a", unpacked.exec_ctrl.pred_rf_raddr_a, ctrl.exec_ctrl.pred_rf_raddr_a);
    `EXPECT_FIELD("roundtrip pred_rf_raddr_b", unpacked.exec_ctrl.pred_rf_raddr_b, ctrl.exec_ctrl.pred_rf_raddr_b);

    `EXPECT_FIELD("roundtrip data_w0_we", unpacked.data_rf_ctrl.data_w0_we, ctrl.data_rf_ctrl.data_w0_we);
    `EXPECT_FIELD("roundtrip data_w0_addr", unpacked.data_rf_ctrl.data_w0_addr, ctrl.data_rf_ctrl.data_w0_addr);
    `EXPECT_FIELD("roundtrip data_w1_we", unpacked.data_rf_ctrl.data_w1_we, ctrl.data_rf_ctrl.data_w1_we);
    `EXPECT_FIELD("roundtrip data_w1_addr", unpacked.data_rf_ctrl.data_w1_addr, ctrl.data_rf_ctrl.data_w1_addr);
    `EXPECT_FIELD("roundtrip data_w1_src", int'(unpacked.data_rf_ctrl.data_w1_src), int'(ctrl.data_rf_ctrl.data_w1_src));

    `EXPECT_FIELD("roundtrip pred_w0_we", unpacked.pred_rf_ctrl.pred_w0_we, ctrl.pred_rf_ctrl.pred_w0_we);
    `EXPECT_FIELD("roundtrip pred_w0_addr", unpacked.pred_rf_ctrl.pred_w0_addr, ctrl.pred_rf_ctrl.pred_w0_addr);
    `EXPECT_FIELD("roundtrip pred_w1_we", unpacked.pred_rf_ctrl.pred_w1_we, ctrl.pred_rf_ctrl.pred_w1_we);
    `EXPECT_FIELD("roundtrip pred_w1_addr", unpacked.pred_rf_ctrl.pred_w1_addr, ctrl.pred_rf_ctrl.pred_w1_addr);
    `EXPECT_FIELD("roundtrip pred_w1_src", int'(unpacked.pred_rf_ctrl.pred_w1_src), int'(ctrl.pred_rf_ctrl.pred_w1_src));

    `EXPECT_FIELD("roundtrip data north we", unpacked.data_route_ctrl.north.we, ctrl.data_route_ctrl.north.we);
    `EXPECT_FIELD("roundtrip data north src", int'(unpacked.data_route_ctrl.north.src), int'(ctrl.data_route_ctrl.north.src));
    `EXPECT_FIELD("roundtrip data south we", unpacked.data_route_ctrl.south.we, ctrl.data_route_ctrl.south.we);
    `EXPECT_FIELD("roundtrip data south src", int'(unpacked.data_route_ctrl.south.src), int'(ctrl.data_route_ctrl.south.src));
    `EXPECT_FIELD("roundtrip data east we", unpacked.data_route_ctrl.east.we, ctrl.data_route_ctrl.east.we);
    `EXPECT_FIELD("roundtrip data east src", int'(unpacked.data_route_ctrl.east.src), int'(ctrl.data_route_ctrl.east.src));
    `EXPECT_FIELD("roundtrip data west we", unpacked.data_route_ctrl.west.we, ctrl.data_route_ctrl.west.we);
    `EXPECT_FIELD("roundtrip data west src", int'(unpacked.data_route_ctrl.west.src), int'(ctrl.data_route_ctrl.west.src));

    `EXPECT_FIELD("roundtrip pred north we", unpacked.pred_route_ctrl.north.we, ctrl.pred_route_ctrl.north.we);
    `EXPECT_FIELD("roundtrip pred north src", int'(unpacked.pred_route_ctrl.north.src), int'(ctrl.pred_route_ctrl.north.src));
    `EXPECT_FIELD("roundtrip pred south we", unpacked.pred_route_ctrl.south.we, ctrl.pred_route_ctrl.south.we);
    `EXPECT_FIELD("roundtrip pred south src", int'(unpacked.pred_route_ctrl.south.src), int'(ctrl.pred_route_ctrl.south.src));
    `EXPECT_FIELD("roundtrip pred east we", unpacked.pred_route_ctrl.east.we, ctrl.pred_route_ctrl.east.we);
    `EXPECT_FIELD("roundtrip pred east src", int'(unpacked.pred_route_ctrl.east.src), int'(ctrl.pred_route_ctrl.east.src));
    `EXPECT_FIELD("roundtrip pred west we", unpacked.pred_route_ctrl.west.we, ctrl.pred_route_ctrl.west.we);
    `EXPECT_FIELD("roundtrip pred west src", int'(unpacked.pred_route_ctrl.west.src), int'(ctrl.pred_route_ctrl.west.src));

    `EXPECT_FIELD("roundtrip const_addr", unpacked.const_ctrl.const_addr, ctrl.const_ctrl.const_addr);
    `EXPECT_FIELD("roundtrip lsu_op", int'(unpacked.lsu_ctrl.lsu_op), int'(ctrl.lsu_ctrl.lsu_op));
    `EXPECT_FIELD("roundtrip lsu_addr_src", int'(unpacked.lsu_ctrl.lsu_addr_src), int'(ctrl.lsu_ctrl.lsu_addr_src));
    `EXPECT_FIELD("roundtrip lsu_store_data_src", int'(unpacked.lsu_ctrl.lsu_store_data_src), int'(ctrl.lsu_ctrl.lsu_store_data_src));
    `EXPECT_FIELD("roundtrip lsu_commit_pred_enable", unpacked.lsu_ctrl.lsu_commit_pred_enable, ctrl.lsu_ctrl.lsu_commit_pred_enable);
    `EXPECT_FIELD("roundtrip lsu_commit_pred_invert", unpacked.lsu_ctrl.lsu_commit_pred_invert, ctrl.lsu_ctrl.lsu_commit_pred_invert);
    `EXPECT_FIELD("roundtrip lsu_commit_pred_src", int'(unpacked.lsu_ctrl.lsu_commit_pred_src), int'(ctrl.lsu_ctrl.lsu_commit_pred_src));

    expect_int("EXEC_CTRL_LSB", EXEC_CTRL_LSB, 0);
    expect_int("DATA_RF_CTRL_LSB", DATA_RF_CTRL_LSB, 38);
    expect_int("PRED_RF_CTRL_LSB", PRED_RF_CTRL_LSB, 52);
    expect_int("DATA_ROUTE_CTRL_LSB", DATA_ROUTE_CTRL_LSB, 66);
    expect_int("PRED_ROUTE_CTRL_LSB", PRED_ROUTE_CTRL_LSB, 86);
    expect_int("CONST_CTRL_LSB", CONST_CTRL_LSB, 106);
    expect_int("LSU_CTRL_LSB", LSU_CTRL_LSB, 110);
    expect_int("CONTROL_WORD_PADDING_LSB", CONTROL_WORD_PADDING_LSB, 126);
    expect_int("CONTROL_WORD_PADDING_WIDTH", CONTROL_WORD_PADDING_WIDTH, 2);

    expected_bits = '0;
    expected_bits[EXEC_CTRL_LSB +: EXEC_CTRL_WIDTH] = pack_exec_ctrl(ctrl.exec_ctrl);
    expected_bits[DATA_RF_CTRL_LSB +: DATA_RF_CTRL_WIDTH] = pack_data_rf_ctrl(ctrl.data_rf_ctrl);
    expected_bits[PRED_RF_CTRL_LSB +: PRED_RF_CTRL_WIDTH] = pack_pred_rf_ctrl(ctrl.pred_rf_ctrl);
    expected_bits[DATA_ROUTE_CTRL_LSB +: DATA_ROUTE_CTRL_WIDTH] = pack_data_route_ctrl(ctrl.data_route_ctrl);
    expected_bits[PRED_ROUTE_CTRL_LSB +: PRED_ROUTE_CTRL_WIDTH] = pack_pred_route_ctrl(ctrl.pred_route_ctrl);
    expected_bits[CONST_CTRL_LSB +: CONST_CTRL_WIDTH] = pack_const_ctrl(ctrl.const_ctrl);
    expected_bits[LSU_CTRL_LSB +: LSU_CTRL_WIDTH] = pack_lsu_ctrl(ctrl.lsu_ctrl);
    expect_bits("field slices", bits, expected_bits);
    `EXPECT_FIELD("padding bit 126", bits[126], 0);
    `EXPECT_FIELD("padding bit 127", bits[127], 0);

    $display("Control word pack/unpack test passed");
    $finish;
  end
endmodule : control_word_tb
