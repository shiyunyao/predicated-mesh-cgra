// SPDX-License-Identifier: MIT
#include "Vtop.h"
#include "verilated.h"

double sc_time_stamp() {
    return 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    Vtop* top = new Vtop;
    top->clk = 0;
    while (!Verilated::gotFinish()) {
        top->clk = 0;
        top->eval();
        top->clk = 1;
        top->eval();
    }
    delete top;
    return 0;
}
