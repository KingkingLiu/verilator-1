#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2022 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(dist => 1);
if ($ENV{VERILATOR_TEST_NO_ATTRIBUTES}) {
    skip("Skipping due to VERILATOR_TEST_NO_ATTRIBUTES");
} else {
    check();
}
sub check {
    my $root = "..";
    # some of the files are only used in verilation
    # and are only in "include" folder
    my $input_dirs = "$root/test_regress/t/";
    my $clang_args = "-I$root/include";
    # don't check symbols that starts with
    my $exclude = "std::,__builtin_,__gnu_cxx";


    sub run_clang_check {
	{
	    my $cmd = qq{python3 -c "from clang.cindex import Index; index = Index.create(); print(\\"Clang imported\\")";};
	    print "\t$cmd\n" if $::Debug;
	    my $out = `$cmd`;
	    if (!$out || $out !~ /Clang imported/) { skip("No libclang installed\n"); return 1; }
	}
	run(logfile => $Self->{run_log_filename},
	    tee => 1,
	    cmd => ["python3", "$root/nodist/clang_check_attributes $input_dirs $clang_args -x $exclude -filter t_dist_attributes_bad -print-all"]);

	files_identical($Self->{run_log_filename}, $Self->{golden_filename});
    }

    run_clang_check();
}

ok(1);
1;
