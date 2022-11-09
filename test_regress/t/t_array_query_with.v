// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t (/*AUTOARG*/
      clk
   );

   input clk;
   
   function int get_1;
      string bar[$];
      string found[$];
      bar.push_back("baz");
      bar.push_back("qux");
      found = bar.find(x) with (x == "baz");
      return found.size();
   endfunction

   always @(posedge clk) begin
      if (get_1() == 1) begin
         $write("*-* All Finished *-*\n");
         $finish;
      end
      else
         $stop;
   end
endmodule
