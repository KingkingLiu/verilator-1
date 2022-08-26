// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
    localparam N = 100;

    event e[N];

    generate for (genvar i = 0; i < N-1; i++)
        always @e[i] ->e[i+1];
    endgenerate

    initial ->e[0];

    always @e[N-1] begin
        for (int i = 0; i < N; i++) if (!e[i].triggered) $stop;
        $write("*-* All Finished *-*\n");
        $finish;
    end
endmodule
