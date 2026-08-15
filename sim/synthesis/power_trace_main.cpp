// SPDX-License-Identifier: MIT
#include "Vtop.h"
#include "verilated.h"
#include "verilated_saif_c.h"

#include <cstring>

namespace {

vluint64_t main_time_ps = 0;
constexpr vluint64_t kHalfPeriodPs = 5000;
constexpr const char* kSaifArgument = "+CGRA_POWER_SAIF=";

const char* saif_path(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strncmp(argv[index], kSaifArgument, std::strlen(kSaifArgument)) == 0) {
            return argv[index] + std::strlen(kSaifArgument);
        }
    }
    return "reports/synthesis/raw/asap7_power_activity_small.saif";
}

}  // namespace

double sc_time_stamp() {
    return static_cast<double>(main_time_ps);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    auto* top = new Vtop;
    auto* trace = new VerilatedSaifC;
    top->trace(trace, 99);
    trace->open(saif_path(argc, argv));

    top->clk = 0;
    while (!Verilated::gotFinish()) {
        top->clk = 0;
        top->eval();
        trace->dump(main_time_ps);
        main_time_ps += kHalfPeriodPs;

        top->clk = 1;
        top->eval();
        trace->dump(main_time_ps);
        main_time_ps += kHalfPeriodPs;
    }

    top->final();
    trace->close();
    delete trace;
    delete top;
    return 0;
}
