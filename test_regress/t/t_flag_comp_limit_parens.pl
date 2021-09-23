#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2008 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(vlt => 1);

compile(
    verilator_flags2 => ["--comp-limit-parens 2"],
    );

execute(
    check_finished => 1,
    );

my $Vdeeptemp = 0;
my $globbed = "$Self->{obj_dir}/Vt_flag_comp_limit_parens___024root__DepSet_*__0__Slow.cpp";
foreach my $file (glob_all($globbed)) {
    my $text = file_contents($file);
    $Vdeeptemp |= $text =~ qr/Vdeeptemp/x;
}
error("$globbed does not have 'Vdeeptemp'") if !$Vdeeptemp;

ok(1);
1;
