// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t (/*AUTOARG*/
      clk
   );

   input clk;

   class foo;
      function int get_1;
         return 1;
      endfunction
   endclass

   class bar #(type T=foo) extends T;
   endclass

   bar bar_foo_i;
   initial bar_foo_i = new;

   class baz;
      function int get_2;
         return 2;
      endfunction
   endclass

   bar #(baz) bar_baz_i;
   initial bar_baz_i = new;

   always @(posedge clk) begin
      if (bar_foo_i.get_1() == 1 && bar_baz_i.get_2() == 2) begin
         $write("*-* All Finished *-*\n");
         $finish;
      end
      else begin
         $stop;
      end
   end
endmodule
