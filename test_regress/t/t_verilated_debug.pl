#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2003 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(vlt_all => 1);

$Self->{verilated_debug} = 1;

compile(
    verilator_flags2 => [],
    );

execute(
    check_finished => 1,
    );

if (!$Self->{vltmt}) {  # vltmt output may vary between thread exec order
    files_identical("$Self->{obj_dir}/vlt_sim.log", $Self->{dynamic_scheduler} ? $Self->{golden_filename} =~ s/\.out$/_dsched.out/r : $Self->{golden_filename}, "logfile");
}

ok(1);
1;
