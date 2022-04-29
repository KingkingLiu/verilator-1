// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2022 by Antmicro Ltd.
// SPDX-License-Identifier: CC0-1.0

module t;
   event e1;
   event e2;
   event e3;

   initial begin
      // p1->e2  ==>  p2->e3  ==>  p3->e3  ==>  p2->e2  ==>  p1->e3  ==>  p3->e1
      fork
         begin
            #1 $write("forked process 1\n");
            ->e2;
            @e2 $write("forked process 1 again\n");
            ->e3;
         end
         begin
            @e2 $write("forked process 2\n");
            ->e3;
            @e3 $write("forked process 2 again\n");
            ->e2;
         end
         begin
            @e3 $write("forked process 3\n");
            ->e3;
            @e3 $write("forked process 3 again\n");
            ->e1;
         end
      join_none
      $write("in main process\n");
      @e1;
      $write("*-* All Finished *-*\n");
      $finish;
    end
endmodule
