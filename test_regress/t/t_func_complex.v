// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2020 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module t();
   typedef integer q_t[$];

   function void queue_set(ref q_t q);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      q.push_back(42);
   endfunction

   function void queue_check_nref(q_t q);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      q[0] = 11;
      if (q[0] != 11) $stop;
   endfunction

   function void queue_check_ref(const ref q_t q);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      if (q[0] != 42) $stop;
   endfunction

   function q_t queue_ret();
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      queue_ret = '{101};
   endfunction

   function int get_1_push(ref q_t q);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      q.push_back(1);
      return 1;
   endfunction

   function int get_2_push(ref q_t q);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      q.push_back(2);
      return 2;
   endfunction

   function void check_sizes(int cond);
`ifdef TEST_NOINLINE
      // verilator no_inline_task
`endif
      q_t q;
      int func_call_result;
      if (cond < 1)
        func_call_result = get_1_push(q);
      else
        func_call_result = get_2_push(q);

      if (q.size() != 1) $stop;
   endfunction

   initial begin
      q_t iq;
      queue_set(iq);
      queue_check_ref(iq);

      iq[0] = 44;
      queue_check_nref(iq);
      if (iq[0] != 44) $stop;

      iq = queue_ret();
      if (iq[0] != 101) $stop;

      check_sizes(2);

      $write("*-* All Finished *-*\n");
      $finish;
   end
endmodule
