#!/usr/bin/perl
#
# Host-side regression tests for the pure helper subs in 98_CasambiGW.pm.
# Loads the module with `do` (the same mechanism FHEM uses) and exercises the
# percent->byte conversion and the refresh-response classifier. No FHEM runtime
# is required. Run: perl FHEM/t/CasambiGW_helpers.t
use strict;
use warnings;
use Test::More tests => 17;
use FindBin qw($RealBin);

# Load the module body into package main (defines the subs).
my $module = "$RealBin/../98_CasambiGW.pm";
do $module;
die "failed to load $module: $@" if $@;

# ---- CasambiGW_PercentToByte: exact endpoints and rounding -------------------
is(main::CasambiGW_PercentToByte(0),   0,   '0% -> 0');
is(main::CasambiGW_PercentToByte(1),   3,   '1% -> 3 (rounded)');
is(main::CasambiGW_PercentToByte(50),  128, '50% -> 128 (rounded)');
is(main::CasambiGW_PercentToByte(100), 255, '100% -> 255 (not 254!)');

# Clamping of out-of-range input.
is(main::CasambiGW_PercentToByte(-5),  0,   'negative clamps to 0');
is(main::CasambiGW_PercentToByte(150), 255, '>100 clamps to 255');
is(main::CasambiGW_PercentToByte(undef), 0, 'undef -> 0');

# Output stays within a valid byte over the whole 0..100 range, and is monotone.
my $prev = -1;
my $ok_range = 1;
my $ok_mono  = 1;
for my $p (0 .. 100) {
    my $b = main::CasambiGW_PercentToByte($p);
    $ok_range = 0 if $b < 0 || $b > 255;
    $ok_mono  = 0 if $b < $prev;
    $prev = $b;
}
ok($ok_range, 'all outputs within 0..255');
ok($ok_mono,  'mapping is monotone over 0..100');

# ---- CasambiGW_ClassifyRefreshResponse --------------------------------------
is(main::CasambiGW_ClassifyRefreshResponse('read timeout', 0), 'reboot',
   'transport error, no HTTP status -> reboot (expected)');
is(main::CasambiGW_ClassifyRefreshResponse('', 200), 'accepted', 'HTTP 200 -> accepted');
is(main::CasambiGW_ClassifyRefreshResponse('', 202), 'accepted', 'HTTP 202 -> accepted');
is(main::CasambiGW_ClassifyRefreshResponse('', 401), 'auth',     'HTTP 401 -> auth');
is(main::CasambiGW_ClassifyRefreshResponse('', 403), 'auth',     'HTTP 403 -> auth');
is(main::CasambiGW_ClassifyRefreshResponse('', 409), 'conflict', 'HTTP 409 -> conflict');
is(main::CasambiGW_ClassifyRefreshResponse('', 500), 'gatewayerror', 'HTTP 500 -> gatewayerror');
# A status accompanied by a transport error must still be classified by status,
# never silently reported as accepted.
is(main::CasambiGW_ClassifyRefreshResponse('partial', 500), 'gatewayerror',
   'HTTP 500 with transport error -> gatewayerror (not reboot/accepted)');
