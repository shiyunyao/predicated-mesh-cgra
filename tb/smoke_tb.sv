// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module smoke_tb;
  import cgra_pkg::*;

  initial begin
    $display("CGRA scaffold smoke test passed");
    $finish;
  end
endmodule : smoke_tb
