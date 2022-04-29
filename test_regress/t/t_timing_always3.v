// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
   logic clk = 0;
   always #3 clk = ~clk;

   logic flag_a;
   logic flag_b;
   always @(posedge clk)
   begin
      $display("[%0t] b <= 0", $time);
      flag_b <= 1'b0;
      #2
      $display("[%0t] a <= 1", $time);
      flag_a <= 1'b1;
      #2
      $display("[%0t] b <= 1", $time);
      flag_b <= 1'b1;
   end
   always @(flag_a)
   begin
      #1
      $display("[%0t] Checking if b == 0", $time);
      if (flag_b !== 1'b0) $stop;
      #2
      $display("[%0t] Checking if b == 1", $time);
      if (flag_b !== 1'b1) $stop;
      #10
      $write("*-* All Finished *-*\n");
      $finish;
   end
endmodule
