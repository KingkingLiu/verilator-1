// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
   initial begin
      fork
         #8 $write("[%0t] forked process 1\n", $time);
         #4 $write("[%0t] forked process 2\n", $time);
         #2 $write("[%0t] forked process 3\n", $time);
         $write("[%0t] forked process 4\n", $time);
      join
      #8 $write("[%0t] main process\n", $time);
      $write("*-* All Finished *-*\n");
      $finish;
    end
endmodule
