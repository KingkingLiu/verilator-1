// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2009 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module x;
   typedef struct {
      int         a;
   } embedded_t;

   typedef struct {
      embedded_t b;
   } notembedded_t;

   notembedded_t p;

   initial begin
      p.b.a = 1;
      if (p.b.a != 1) $stop;
      $write("*-* All Finished *-*\n");
      $finish;
   end
endmodule
