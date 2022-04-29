// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
   event e;

   initial begin
      fork
         $write("forked process 1\n");
         begin
            @e;
            $write("forked process 2\n");
            ->e;
         end
      join_any
      $write("back in main process\n");
      ->e;
      @e;
      $write("*-* All Finished *-*\n");
      $finish;
    end
endmodule
