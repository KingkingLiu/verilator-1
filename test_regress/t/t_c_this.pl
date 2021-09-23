#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2021 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(vlt => 1);

compile();

if ($Self->{vlt_all}) {
    # The word 'this' (but only the whole word 'this' should have been replaced
    # in the contents.
    my $thisclk = 0; my $xthis = 0; my $thisx = 0; my $xthisx = 0;
    my $globbed = "$Self->{obj_dir}/$Self->{VM_PREFIX}___024root__DepSet_*__0.cpp";
    foreach my $file (glob_all($globbed)) {
        my $text = file_contents($file);
        $thisclk |= $text =~ m/\bthis->clk\b/;
        $xthis |= $text =~ m/\bxthis\b/;
        $thisx |= $text =~ m/\bthisx\b/;
        $xthisx |= $text =~ m/\bxthisx\b/;
    }
    error("$globbed has 'this->clk'") if $thisclk;
    error("$globbed does not have 'xthis'") if !$xthis;
    error("$globbed does not have 'thisx'") if !$thisx;
    error("$globbed does not have 'xthisx'") if !$xthisx;
}

ok(1);
1;
