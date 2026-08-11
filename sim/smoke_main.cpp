// SPDX-License-Identifier: MIT
#include "Vtop.h"
#include "verilated.h"

double sc_time_stamp() {
    return 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    Vtop* top = new Vtop;
    while (!Verilated::gotFinish()) {
        top->eval();
    }
    delete top;
    return 0;
}
