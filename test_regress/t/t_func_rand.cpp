// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2006 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>
#include "Vt_func_rand.h"

double simTime = 0;
double sc_time_stamp() { return simTime; }

int main(int argc, char* argv[]) {
    Vt_func_rand* topp = new Vt_func_rand;

    Verilated::debug(0);

    printf("\nTesting\n");
    topp->clk = 0;
    for (int i = 0; i < 10; i++) {
#ifdef VL_DYNAMIC_SCHEDULER
        topp->eval();
        auto newTime = topp->nextTimeSlot();
        if (newTime - simTime <= 0 ||
            newTime - floorf(newTime) == 0) {
            topp->clk = !topp->clk;
            simTime += 1;
        } else {
            simTime = newTime;
        }
#else
        topp->eval();
        topp->clk = 1;
        topp->eval();
        topp->clk = 0;
#endif
    }
    if (topp->Rand != 0xfeed0fad) {
        vl_fatal(__FILE__, __LINE__, "top", "Unexpected value for Rand output\n");
    }
    topp->final();
    VL_DO_DANGLING(delete topp, topp);
    printf("*-* All Finished *-*\n");
}
