// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

package cgra_pkg;
  function automatic int log2ceil(input int value);
    int result;
    int shifted;
    begin
      result = 0;
      shifted = value - 1;
      while (shifted > 0) begin
        result = result + 1;
        shifted = shifted >> 1;
      end
      log2ceil = result;
    end
  endfunction

  function automatic int round_up(input int value, input int align);
    begin
      round_up = ((value + align - 1) / align) * align;
    end
  endfunction

  localparam int ARRAY_ROWS = 4;
  localparam int ARRAY_COLS = 4;

  localparam int DATA_WIDTH = 32;
  localparam int PRED_WIDTH = 1;

  localparam int DATA_RF_DEPTH = 16;
  localparam int PRED_RF_DEPTH = 16;

  localparam int CONST_MEM_DEPTH = 16;
  localparam int CTRL_MEM_DEPTH = 256;

  localparam int SCRATCH_BANK_DEPTH = 1024;

  localparam int MESH_HOP_LATENCY = 1;
  localparam int LOAD_LATENCY = 2;

  localparam int CONTROL_WORD_ALIGN = 32;

  localparam int DATA_RF_ADDR_WIDTH = log2ceil(DATA_RF_DEPTH);
  localparam int PRED_RF_ADDR_WIDTH = log2ceil(PRED_RF_DEPTH);
  localparam int CONST_ADDR_WIDTH = log2ceil(CONST_MEM_DEPTH);
  localparam int CTRL_PC_WIDTH = log2ceil(CTRL_MEM_DEPTH);
  localparam int SCRATCH_ADDR_WIDTH = log2ceil(SCRATCH_BANK_DEPTH);

  localparam int OP_WIDTH = 6;
  localparam int DATA_SRC_WIDTH = 4;
  localparam int PRED_SRC_WIDTH = 4;
  localparam int DATA_W1_SRC_WIDTH = 4;
  localparam int PRED_W1_SRC_WIDTH = 4;
  localparam int DATA_ROUTE_SRC_WIDTH = 4;
  localparam int PRED_ROUTE_SRC_WIDTH = 4;
  localparam int LSU_OP_WIDTH = 2;

  localparam int EXEC_CTRL_WIDTH = OP_WIDTH
                                   + DATA_SRC_WIDTH
                                   + DATA_SRC_WIDTH
                                   + PRED_SRC_WIDTH
                                   + PRED_SRC_WIDTH
                                   + DATA_RF_ADDR_WIDTH
                                   + DATA_RF_ADDR_WIDTH
                                   + PRED_RF_ADDR_WIDTH
                                   + PRED_RF_ADDR_WIDTH;
  localparam int DATA_RF_CTRL_WIDTH = 1
                                      + DATA_RF_ADDR_WIDTH
                                      + 1
                                      + DATA_RF_ADDR_WIDTH
                                      + DATA_W1_SRC_WIDTH;
  localparam int PRED_RF_CTRL_WIDTH = 1
                                      + PRED_RF_ADDR_WIDTH
                                      + 1
                                      + PRED_RF_ADDR_WIDTH
                                      + PRED_W1_SRC_WIDTH;
  localparam int DATA_ROUTE_DIR_CTRL_WIDTH = 1 + DATA_ROUTE_SRC_WIDTH;
  localparam int PRED_ROUTE_DIR_CTRL_WIDTH = 1 + PRED_ROUTE_SRC_WIDTH;
  localparam int DATA_ROUTE_CTRL_WIDTH = 4 * DATA_ROUTE_DIR_CTRL_WIDTH;
  localparam int PRED_ROUTE_CTRL_WIDTH = 4 * PRED_ROUTE_DIR_CTRL_WIDTH;
  localparam int CONST_CTRL_WIDTH = CONST_ADDR_WIDTH;
  localparam int LSU_CTRL_WIDTH = LSU_OP_WIDTH
                                  + DATA_SRC_WIDTH
                                  + DATA_SRC_WIDTH
                                  + 1
                                  + 1
                                  + PRED_SRC_WIDTH;

  localparam int CONTROL_WORD_RAW_WIDTH = EXEC_CTRL_WIDTH
                                          + DATA_RF_CTRL_WIDTH
                                          + PRED_RF_CTRL_WIDTH
                                          + DATA_ROUTE_CTRL_WIDTH
                                          + PRED_ROUTE_CTRL_WIDTH
                                          + CONST_CTRL_WIDTH
                                          + LSU_CTRL_WIDTH;
  localparam int CONTROL_WORD_PHYSICAL_WIDTH = round_up(CONTROL_WORD_RAW_WIDTH,
                                                        CONTROL_WORD_ALIGN);
  localparam int CONTROL_WORD_CHUNKS = CONTROL_WORD_PHYSICAL_WIDTH / 32;

  typedef enum logic [OP_WIDTH-1:0] {
    OP_NOP     = 6'd0,
    OP_PASS    = 6'd1,
    OP_ADD     = 6'd2,
    OP_SUB     = 6'd3,
    OP_MUL     = 6'd4,
    OP_AND     = 6'd5,
    OP_OR      = 6'd6,
    OP_XOR     = 6'd7,
    OP_SHL     = 6'd8,
    OP_LSHR    = 6'd9,
    OP_SELECT  = 6'd10,
    OP_CMP_EQ  = 6'd16,
    OP_CMP_NE  = 6'd17,
    OP_CMP_ULT = 6'd18,
    OP_CMP_ULE = 6'd19,
    OP_PPASS   = 6'd32,
    OP_PNOT    = 6'd33,
    OP_PAND    = 6'd34,
    OP_POR     = 6'd35
  } op_e;

  typedef enum logic [DATA_SRC_WIDTH-1:0] {
    DATA_SRC_RF_A          = 4'd0,
    DATA_SRC_RF_B          = 4'd1,
    DATA_SRC_NORTH_DATA_IN = 4'd2,
    DATA_SRC_SOUTH_DATA_IN = 4'd3,
    DATA_SRC_EAST_DATA_IN  = 4'd4,
    DATA_SRC_WEST_DATA_IN  = 4'd5,
    DATA_SRC_CONST_DATA    = 4'd6,
    DATA_SRC_LSU_LOAD_DATA = 4'd7,
    DATA_SRC_ZERO          = 4'd8
  } data_src_e;

  typedef enum logic [PRED_SRC_WIDTH-1:0] {
    PRED_SRC_RF_A          = 4'd0,
    PRED_SRC_RF_B          = 4'd1,
    PRED_SRC_NORTH_PRED_IN = 4'd2,
    PRED_SRC_SOUTH_PRED_IN = 4'd3,
    PRED_SRC_EAST_PRED_IN  = 4'd4,
    PRED_SRC_WEST_PRED_IN  = 4'd5,
    PRED_SRC_CONST_TRUE    = 4'd6,
    PRED_SRC_CONST_FALSE   = 4'd7
  } pred_src_e;

  typedef enum logic [DATA_ROUTE_SRC_WIDTH-1:0] {
    DATA_ROUTE_SRC_NONE          = 4'd0,
    DATA_ROUTE_SRC_NORTH_DATA_IN = 4'd1,
    DATA_ROUTE_SRC_SOUTH_DATA_IN = 4'd2,
    DATA_ROUTE_SRC_EAST_DATA_IN  = 4'd3,
    DATA_ROUTE_SRC_WEST_DATA_IN  = 4'd4,
    DATA_ROUTE_SRC_FU_DATA       = 4'd5,
    DATA_ROUTE_SRC_RF_A          = 4'd6,
    DATA_ROUTE_SRC_RF_B          = 4'd7,
    DATA_ROUTE_SRC_CONST_DATA    = 4'd8,
    DATA_ROUTE_SRC_LSU_LOAD_DATA = 4'd9,
    DATA_ROUTE_SRC_ZERO          = 4'd10
  } data_route_src_e;

  typedef enum logic [PRED_ROUTE_SRC_WIDTH-1:0] {
    PRED_ROUTE_SRC_NONE          = 4'd0,
    PRED_ROUTE_SRC_NORTH_PRED_IN = 4'd1,
    PRED_ROUTE_SRC_SOUTH_PRED_IN = 4'd2,
    PRED_ROUTE_SRC_EAST_PRED_IN  = 4'd3,
    PRED_ROUTE_SRC_WEST_PRED_IN  = 4'd4,
    PRED_ROUTE_SRC_FU_PRED       = 4'd5,
    PRED_ROUTE_SRC_RF_A          = 4'd6,
    PRED_ROUTE_SRC_RF_B          = 4'd7,
    PRED_ROUTE_SRC_CONST_TRUE    = 4'd8,
    PRED_ROUTE_SRC_CONST_FALSE   = 4'd9
  } pred_route_src_e;

  typedef enum logic [LSU_OP_WIDTH-1:0] {
    LSU_OP_NONE     = 2'd0,
    LSU_OP_LOAD     = 2'd1,
    LSU_OP_STORE    = 2'd2,
    LSU_OP_RESERVED = 2'd3
  } lsu_op_e;

  localparam int EXEC_CTRL_LSB = 0;
  localparam int DATA_RF_CTRL_LSB = EXEC_CTRL_LSB + EXEC_CTRL_WIDTH;
  localparam int PRED_RF_CTRL_LSB = DATA_RF_CTRL_LSB + DATA_RF_CTRL_WIDTH;
  localparam int DATA_ROUTE_CTRL_LSB = PRED_RF_CTRL_LSB + PRED_RF_CTRL_WIDTH;
  localparam int PRED_ROUTE_CTRL_LSB = DATA_ROUTE_CTRL_LSB + DATA_ROUTE_CTRL_WIDTH;
  localparam int CONST_CTRL_LSB = PRED_ROUTE_CTRL_LSB + PRED_ROUTE_CTRL_WIDTH;
  localparam int LSU_CTRL_LSB = CONST_CTRL_LSB + CONST_CTRL_WIDTH;
  localparam int CONTROL_WORD_PADDING_LSB = CONTROL_WORD_RAW_WIDTH;
  localparam int CONTROL_WORD_PADDING_WIDTH = CONTROL_WORD_PHYSICAL_WIDTH
                                              - CONTROL_WORD_RAW_WIDTH;

  typedef logic [CONTROL_WORD_PHYSICAL_WIDTH-1:0] control_word_bits_t;
  typedef logic [EXEC_CTRL_WIDTH-1:0] exec_ctrl_bits_t;
  typedef logic [DATA_RF_CTRL_WIDTH-1:0] data_rf_ctrl_bits_t;
  typedef logic [PRED_RF_CTRL_WIDTH-1:0] pred_rf_ctrl_bits_t;
  typedef logic [DATA_ROUTE_CTRL_WIDTH-1:0] data_route_ctrl_bits_t;
  typedef logic [PRED_ROUTE_CTRL_WIDTH-1:0] pred_route_ctrl_bits_t;
  typedef logic [CONST_CTRL_WIDTH-1:0] const_ctrl_bits_t;
  typedef logic [LSU_CTRL_WIDTH-1:0] lsu_ctrl_bits_t;

  typedef struct packed {
    op_e op;
    data_src_e src_a_sel;
    data_src_e src_b_sel;
    pred_src_e src_p0_sel;
    pred_src_e src_p1_sel;
    logic [DATA_RF_ADDR_WIDTH-1:0] data_rf_raddr_a;
    logic [DATA_RF_ADDR_WIDTH-1:0] data_rf_raddr_b;
    logic [PRED_RF_ADDR_WIDTH-1:0] pred_rf_raddr_a;
    logic [PRED_RF_ADDR_WIDTH-1:0] pred_rf_raddr_b;
  } exec_ctrl_t;

  typedef struct packed {
    logic data_w0_we;
    logic [DATA_RF_ADDR_WIDTH-1:0] data_w0_addr;
    logic data_w1_we;
    logic [DATA_RF_ADDR_WIDTH-1:0] data_w1_addr;
    data_src_e data_w1_src;
  } data_rf_ctrl_t;

  typedef struct packed {
    logic pred_w0_we;
    logic [PRED_RF_ADDR_WIDTH-1:0] pred_w0_addr;
    logic pred_w1_we;
    logic [PRED_RF_ADDR_WIDTH-1:0] pred_w1_addr;
    pred_src_e pred_w1_src;
  } pred_rf_ctrl_t;

  typedef struct packed {
    logic we;
    data_route_src_e src;
  } data_route_dir_ctrl_t;

  typedef struct packed {
    data_route_dir_ctrl_t north;
    data_route_dir_ctrl_t south;
    data_route_dir_ctrl_t east;
    data_route_dir_ctrl_t west;
  } data_route_ctrl_t;

  typedef struct packed {
    logic we;
    pred_route_src_e src;
  } pred_route_dir_ctrl_t;

  typedef struct packed {
    pred_route_dir_ctrl_t north;
    pred_route_dir_ctrl_t south;
    pred_route_dir_ctrl_t east;
    pred_route_dir_ctrl_t west;
  } pred_route_ctrl_t;

  typedef struct packed {
    logic [CONST_ADDR_WIDTH-1:0] const_addr;
  } const_ctrl_t;

  typedef struct packed {
    lsu_op_e lsu_op;
    data_src_e lsu_addr_src;
    data_src_e lsu_store_data_src;
    logic lsu_commit_pred_enable;
    logic lsu_commit_pred_invert;
    pred_src_e lsu_commit_pred_src;
  } lsu_ctrl_t;

  typedef struct packed {
    exec_ctrl_t exec_ctrl;
    data_rf_ctrl_t data_rf_ctrl;
    pred_rf_ctrl_t pred_rf_ctrl;
    data_route_ctrl_t data_route_ctrl;
    pred_route_ctrl_t pred_route_ctrl;
    const_ctrl_t const_ctrl;
    lsu_ctrl_t lsu_ctrl;
  } tile_control_word_t;

  function automatic exec_ctrl_bits_t pack_exec_ctrl(input exec_ctrl_t ctrl);
    begin
      pack_exec_ctrl = '0;
      pack_exec_ctrl[0 +: OP_WIDTH] = ctrl.op;
      pack_exec_ctrl[OP_WIDTH +: DATA_SRC_WIDTH] = ctrl.src_a_sel;
      pack_exec_ctrl[OP_WIDTH + DATA_SRC_WIDTH +: DATA_SRC_WIDTH] = ctrl.src_b_sel;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) +: PRED_SRC_WIDTH] = ctrl.src_p0_sel;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) + PRED_SRC_WIDTH +: PRED_SRC_WIDTH] = ctrl.src_p1_sel;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) +: DATA_RF_ADDR_WIDTH] = ctrl.data_rf_raddr_a;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + DATA_RF_ADDR_WIDTH +: DATA_RF_ADDR_WIDTH] = ctrl.data_rf_raddr_b;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + (2 * DATA_RF_ADDR_WIDTH) +: PRED_RF_ADDR_WIDTH] = ctrl.pred_rf_raddr_a;
      pack_exec_ctrl[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + (2 * DATA_RF_ADDR_WIDTH) + PRED_RF_ADDR_WIDTH +: PRED_RF_ADDR_WIDTH] = ctrl.pred_rf_raddr_b;
    end
  endfunction

  function automatic exec_ctrl_t unpack_exec_ctrl(input exec_ctrl_bits_t bits);
    begin
      unpack_exec_ctrl.op = op_e'(bits[0 +: OP_WIDTH]);
      unpack_exec_ctrl.src_a_sel = data_src_e'(bits[OP_WIDTH +: DATA_SRC_WIDTH]);
      unpack_exec_ctrl.src_b_sel = data_src_e'(bits[OP_WIDTH + DATA_SRC_WIDTH +: DATA_SRC_WIDTH]);
      unpack_exec_ctrl.src_p0_sel = pred_src_e'(bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) +: PRED_SRC_WIDTH]);
      unpack_exec_ctrl.src_p1_sel = pred_src_e'(bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) + PRED_SRC_WIDTH +: PRED_SRC_WIDTH]);
      unpack_exec_ctrl.data_rf_raddr_a = bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) +: DATA_RF_ADDR_WIDTH];
      unpack_exec_ctrl.data_rf_raddr_b = bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + DATA_RF_ADDR_WIDTH +: DATA_RF_ADDR_WIDTH];
      unpack_exec_ctrl.pred_rf_raddr_a = bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + (2 * DATA_RF_ADDR_WIDTH) +: PRED_RF_ADDR_WIDTH];
      unpack_exec_ctrl.pred_rf_raddr_b = bits[OP_WIDTH + (2 * DATA_SRC_WIDTH) + (2 * PRED_SRC_WIDTH) + (2 * DATA_RF_ADDR_WIDTH) + PRED_RF_ADDR_WIDTH +: PRED_RF_ADDR_WIDTH];
    end
  endfunction

  function automatic data_rf_ctrl_bits_t pack_data_rf_ctrl(input data_rf_ctrl_t ctrl);
    begin
      pack_data_rf_ctrl = '0;
      pack_data_rf_ctrl[0] = ctrl.data_w0_we;
      pack_data_rf_ctrl[1 +: DATA_RF_ADDR_WIDTH] = ctrl.data_w0_addr;
      pack_data_rf_ctrl[1 + DATA_RF_ADDR_WIDTH] = ctrl.data_w1_we;
      pack_data_rf_ctrl[2 + DATA_RF_ADDR_WIDTH +: DATA_RF_ADDR_WIDTH] = ctrl.data_w1_addr;
      pack_data_rf_ctrl[2 + (2 * DATA_RF_ADDR_WIDTH) +: DATA_W1_SRC_WIDTH] = ctrl.data_w1_src;
    end
  endfunction

  function automatic data_rf_ctrl_t unpack_data_rf_ctrl(input data_rf_ctrl_bits_t bits);
    begin
      unpack_data_rf_ctrl.data_w0_we = bits[0];
      unpack_data_rf_ctrl.data_w0_addr = bits[1 +: DATA_RF_ADDR_WIDTH];
      unpack_data_rf_ctrl.data_w1_we = bits[1 + DATA_RF_ADDR_WIDTH];
      unpack_data_rf_ctrl.data_w1_addr = bits[2 + DATA_RF_ADDR_WIDTH +: DATA_RF_ADDR_WIDTH];
      unpack_data_rf_ctrl.data_w1_src = data_src_e'(bits[2 + (2 * DATA_RF_ADDR_WIDTH) +: DATA_W1_SRC_WIDTH]);
    end
  endfunction

  function automatic pred_rf_ctrl_bits_t pack_pred_rf_ctrl(input pred_rf_ctrl_t ctrl);
    begin
      pack_pred_rf_ctrl = '0;
      pack_pred_rf_ctrl[0] = ctrl.pred_w0_we;
      pack_pred_rf_ctrl[1 +: PRED_RF_ADDR_WIDTH] = ctrl.pred_w0_addr;
      pack_pred_rf_ctrl[1 + PRED_RF_ADDR_WIDTH] = ctrl.pred_w1_we;
      pack_pred_rf_ctrl[2 + PRED_RF_ADDR_WIDTH +: PRED_RF_ADDR_WIDTH] = ctrl.pred_w1_addr;
      pack_pred_rf_ctrl[2 + (2 * PRED_RF_ADDR_WIDTH) +: PRED_W1_SRC_WIDTH] = ctrl.pred_w1_src;
    end
  endfunction

  function automatic pred_rf_ctrl_t unpack_pred_rf_ctrl(input pred_rf_ctrl_bits_t bits);
    begin
      unpack_pred_rf_ctrl.pred_w0_we = bits[0];
      unpack_pred_rf_ctrl.pred_w0_addr = bits[1 +: PRED_RF_ADDR_WIDTH];
      unpack_pred_rf_ctrl.pred_w1_we = bits[1 + PRED_RF_ADDR_WIDTH];
      unpack_pred_rf_ctrl.pred_w1_addr = bits[2 + PRED_RF_ADDR_WIDTH +: PRED_RF_ADDR_WIDTH];
      unpack_pred_rf_ctrl.pred_w1_src = pred_src_e'(bits[2 + (2 * PRED_RF_ADDR_WIDTH) +: PRED_W1_SRC_WIDTH]);
    end
  endfunction

  function automatic logic [DATA_ROUTE_DIR_CTRL_WIDTH-1:0] pack_data_route_dir_ctrl(input data_route_dir_ctrl_t ctrl);
    begin
      pack_data_route_dir_ctrl[0] = ctrl.we;
      pack_data_route_dir_ctrl[1 +: DATA_ROUTE_SRC_WIDTH] = ctrl.src;
    end
  endfunction

  function automatic data_route_dir_ctrl_t unpack_data_route_dir_ctrl(input logic [DATA_ROUTE_DIR_CTRL_WIDTH-1:0] bits);
    begin
      unpack_data_route_dir_ctrl.we = bits[0];
      unpack_data_route_dir_ctrl.src = data_route_src_e'(bits[1 +: DATA_ROUTE_SRC_WIDTH]);
    end
  endfunction

  function automatic data_route_ctrl_bits_t pack_data_route_ctrl(input data_route_ctrl_t ctrl);
    begin
      pack_data_route_ctrl = '0;
      pack_data_route_ctrl[0 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH] = pack_data_route_dir_ctrl(ctrl.north);
      pack_data_route_ctrl[1 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH] = pack_data_route_dir_ctrl(ctrl.south);
      pack_data_route_ctrl[2 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH] = pack_data_route_dir_ctrl(ctrl.east);
      pack_data_route_ctrl[3 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH] = pack_data_route_dir_ctrl(ctrl.west);
    end
  endfunction

  function automatic data_route_ctrl_t unpack_data_route_ctrl(input data_route_ctrl_bits_t bits);
    begin
      unpack_data_route_ctrl.north = unpack_data_route_dir_ctrl(bits[0 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH]);
      unpack_data_route_ctrl.south = unpack_data_route_dir_ctrl(bits[1 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH]);
      unpack_data_route_ctrl.east = unpack_data_route_dir_ctrl(bits[2 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH]);
      unpack_data_route_ctrl.west = unpack_data_route_dir_ctrl(bits[3 * DATA_ROUTE_DIR_CTRL_WIDTH +: DATA_ROUTE_DIR_CTRL_WIDTH]);
    end
  endfunction

  function automatic logic [PRED_ROUTE_DIR_CTRL_WIDTH-1:0] pack_pred_route_dir_ctrl(input pred_route_dir_ctrl_t ctrl);
    begin
      pack_pred_route_dir_ctrl[0] = ctrl.we;
      pack_pred_route_dir_ctrl[1 +: PRED_ROUTE_SRC_WIDTH] = ctrl.src;
    end
  endfunction

  function automatic pred_route_dir_ctrl_t unpack_pred_route_dir_ctrl(input logic [PRED_ROUTE_DIR_CTRL_WIDTH-1:0] bits);
    begin
      unpack_pred_route_dir_ctrl.we = bits[0];
      unpack_pred_route_dir_ctrl.src = pred_route_src_e'(bits[1 +: PRED_ROUTE_SRC_WIDTH]);
    end
  endfunction

  function automatic pred_route_ctrl_bits_t pack_pred_route_ctrl(input pred_route_ctrl_t ctrl);
    begin
      pack_pred_route_ctrl = '0;
      pack_pred_route_ctrl[0 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH] = pack_pred_route_dir_ctrl(ctrl.north);
      pack_pred_route_ctrl[1 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH] = pack_pred_route_dir_ctrl(ctrl.south);
      pack_pred_route_ctrl[2 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH] = pack_pred_route_dir_ctrl(ctrl.east);
      pack_pred_route_ctrl[3 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH] = pack_pred_route_dir_ctrl(ctrl.west);
    end
  endfunction

  function automatic pred_route_ctrl_t unpack_pred_route_ctrl(input pred_route_ctrl_bits_t bits);
    begin
      unpack_pred_route_ctrl.north = unpack_pred_route_dir_ctrl(bits[0 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH]);
      unpack_pred_route_ctrl.south = unpack_pred_route_dir_ctrl(bits[1 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH]);
      unpack_pred_route_ctrl.east = unpack_pred_route_dir_ctrl(bits[2 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH]);
      unpack_pred_route_ctrl.west = unpack_pred_route_dir_ctrl(bits[3 * PRED_ROUTE_DIR_CTRL_WIDTH +: PRED_ROUTE_DIR_CTRL_WIDTH]);
    end
  endfunction

  function automatic const_ctrl_bits_t pack_const_ctrl(input const_ctrl_t ctrl);
    begin
      pack_const_ctrl = ctrl.const_addr;
    end
  endfunction

  function automatic const_ctrl_t unpack_const_ctrl(input const_ctrl_bits_t bits);
    begin
      unpack_const_ctrl.const_addr = bits;
    end
  endfunction

  function automatic lsu_ctrl_bits_t pack_lsu_ctrl(input lsu_ctrl_t ctrl);
    begin
      pack_lsu_ctrl = '0;
      pack_lsu_ctrl[0 +: LSU_OP_WIDTH] = ctrl.lsu_op;
      pack_lsu_ctrl[LSU_OP_WIDTH +: DATA_SRC_WIDTH] = ctrl.lsu_addr_src;
      pack_lsu_ctrl[LSU_OP_WIDTH + DATA_SRC_WIDTH +: DATA_SRC_WIDTH] = ctrl.lsu_store_data_src;
      pack_lsu_ctrl[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH)] = ctrl.lsu_commit_pred_enable;
      pack_lsu_ctrl[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH) + 1] = ctrl.lsu_commit_pred_invert;
      pack_lsu_ctrl[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH) + 2 +: PRED_SRC_WIDTH] = ctrl.lsu_commit_pred_src;
    end
  endfunction

  function automatic lsu_ctrl_t unpack_lsu_ctrl(input lsu_ctrl_bits_t bits);
    begin
      unpack_lsu_ctrl.lsu_op = lsu_op_e'(bits[0 +: LSU_OP_WIDTH]);
      unpack_lsu_ctrl.lsu_addr_src = data_src_e'(bits[LSU_OP_WIDTH +: DATA_SRC_WIDTH]);
      unpack_lsu_ctrl.lsu_store_data_src = data_src_e'(bits[LSU_OP_WIDTH + DATA_SRC_WIDTH +: DATA_SRC_WIDTH]);
      unpack_lsu_ctrl.lsu_commit_pred_enable = bits[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH)];
      unpack_lsu_ctrl.lsu_commit_pred_invert = bits[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH) + 1];
      unpack_lsu_ctrl.lsu_commit_pred_src = pred_src_e'(bits[LSU_OP_WIDTH + (2 * DATA_SRC_WIDTH) + 2 +: PRED_SRC_WIDTH]);
    end
  endfunction

  function automatic control_word_bits_t pack_tile_control_word(input tile_control_word_t ctrl);
    begin
      pack_tile_control_word = '0;
      pack_tile_control_word[EXEC_CTRL_LSB +: EXEC_CTRL_WIDTH] = pack_exec_ctrl(ctrl.exec_ctrl);
      pack_tile_control_word[DATA_RF_CTRL_LSB +: DATA_RF_CTRL_WIDTH] = pack_data_rf_ctrl(ctrl.data_rf_ctrl);
      pack_tile_control_word[PRED_RF_CTRL_LSB +: PRED_RF_CTRL_WIDTH] = pack_pred_rf_ctrl(ctrl.pred_rf_ctrl);
      pack_tile_control_word[DATA_ROUTE_CTRL_LSB +: DATA_ROUTE_CTRL_WIDTH] = pack_data_route_ctrl(ctrl.data_route_ctrl);
      pack_tile_control_word[PRED_ROUTE_CTRL_LSB +: PRED_ROUTE_CTRL_WIDTH] = pack_pred_route_ctrl(ctrl.pred_route_ctrl);
      pack_tile_control_word[CONST_CTRL_LSB +: CONST_CTRL_WIDTH] = pack_const_ctrl(ctrl.const_ctrl);
      pack_tile_control_word[LSU_CTRL_LSB +: LSU_CTRL_WIDTH] = pack_lsu_ctrl(ctrl.lsu_ctrl);
    end
  endfunction

  function automatic tile_control_word_t unpack_tile_control_word(input control_word_bits_t bits);
    logic [CONTROL_WORD_PADDING_WIDTH-1:0] padding_bits;
    begin
      padding_bits = bits[CONTROL_WORD_PADDING_LSB +: CONTROL_WORD_PADDING_WIDTH];
      if (^padding_bits === 1'bx) begin
        unpack_tile_control_word = '0;
      end
      unpack_tile_control_word.exec_ctrl = unpack_exec_ctrl(bits[EXEC_CTRL_LSB +: EXEC_CTRL_WIDTH]);
      unpack_tile_control_word.data_rf_ctrl = unpack_data_rf_ctrl(bits[DATA_RF_CTRL_LSB +: DATA_RF_CTRL_WIDTH]);
      unpack_tile_control_word.pred_rf_ctrl = unpack_pred_rf_ctrl(bits[PRED_RF_CTRL_LSB +: PRED_RF_CTRL_WIDTH]);
      unpack_tile_control_word.data_route_ctrl = unpack_data_route_ctrl(bits[DATA_ROUTE_CTRL_LSB +: DATA_ROUTE_CTRL_WIDTH]);
      unpack_tile_control_word.pred_route_ctrl = unpack_pred_route_ctrl(bits[PRED_ROUTE_CTRL_LSB +: PRED_ROUTE_CTRL_WIDTH]);
      unpack_tile_control_word.const_ctrl = unpack_const_ctrl(bits[CONST_CTRL_LSB +: CONST_CTRL_WIDTH]);
      unpack_tile_control_word.lsu_ctrl = unpack_lsu_ctrl(bits[LSU_CTRL_LSB +: LSU_CTRL_WIDTH]);
    end
  endfunction
endpackage : cgra_pkg
