// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
   logic clk = 0;
   always #1 clk = ~clk;

   event e;
   logic sig1 = 0;
   logic sig2 = 1;
   int cyc = 0;
   always @(posedge clk) begin
      cyc <= cyc + 1;
      if (cyc % 5 == 0) ->e;
      else if (cyc % 5 == 1) sig1 = 1;
      else if (cyc % 5 == 2) sig2 = 0;
      else if (cyc % 5 == 3) sig1 = 0;
      else if (cyc % 5 == 4) sig1 = 1;
   end
   initial forever begin
      @(posedge sig1, e)
      $write("POS sig1 or e\n");
      @(posedge sig2, e)
      $write("POS sig2 or e\n");
      @(posedge sig1, negedge sig2)
      $write("POS sig1 or NEG sig2\n");
      @(negedge sig1, posedge sig2)
      $write("NEG sig1 or POS sig2\n");
      $write("*-* All Finished *-*\n");
      $finish;
   end
endmodule
