#!/usr/bin/perl
#
# Host-side regression tests for the pure helper subs in 98_CasambiGW.pm.
# Loads the module with `do` (the same mechanism FHEM uses) and exercises the
# percent->byte conversion and the refresh-response classifier. No FHEM runtime
# is required. Run: perl FHEM/t/CasambiGW_helpers.t
use strict;
use warnings;
use Test::More tests => 31;
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

# ---- CasambiGW_ApiVersionWarning --------------------------------------------
# The module's own version constants (whatever they currently are).
my $maj = main::API_VERSION_MAJOR();
my $min = main::API_VERSION_MINOR();

is(main::CasambiGW_ApiVersionWarning($maj, $min), 'ok', 'exact version match -> ok');
# Missing fields = firmware predating the versioning contract = 1.0. This test
# doubles as a guard: the pre-contract firmware is only compatible while the
# module's major version is 1, so it must be revisited on a major bump.
is(main::CasambiGW_ApiVersionWarning(undef, undef),
   $maj == 1 ? 'ok' : main::CasambiGW_ApiVersionWarning(1, 0),
   'missing fields are treated as 1.0');
# Minor differences are compatible by definition, in both directions.
is(main::CasambiGW_ApiVersionWarning($maj, $min + 1), 'ok', 'ESP minor newer -> ok');
is(main::CasambiGW_ApiVersionWarning($maj, 0),        'ok', 'ESP minor older -> ok');
# Major mismatches warn and name the side that needs the update.
like(main::CasambiGW_ApiVersionWarning($maj + 1, 0), qr/update the FHEM module/,
     'ESP major newer -> warn, FHEM module needs update');
like(main::CasambiGW_ApiVersionWarning($maj - 1, 0), qr/update the ESP32 firmware/,
     'ESP major older -> warn, ESP32 firmware needs update');
like(main::CasambiGW_ApiVersionWarning($maj + 1, 2), qr/\Q$maj.$min\E/,
     'warning names the module version');

# ---- CasambiGW_CasambiVersionWarning ----------------------------------------
is(main::CasambiGW_CasambiVersionWarning(11, 10, 11), 'ok', 'inside range -> ok');
is(main::CasambiGW_CasambiVersionWarning(10, 10, 11), 'ok', 'lower bound -> ok');
is(main::CasambiGW_CasambiVersionWarning(11, 11, 11), 'ok', 'single-version range -> ok');
like(main::CasambiGW_CasambiVersionWarning(9, 10, 11), qr/older than the minimum/,
     'below range -> too-old warning');
like(main::CasambiGW_CasambiVersionWarning(12, 10, 11), qr/newer than the latest tested/,
     'above range -> newer-than-tested warning');
# Old firmware does not report the numbers — never warn on missing input.
is(main::CasambiGW_CasambiVersionWarning(undef, 10, 11), 'ok', 'missing version -> ok');
is(main::CasambiGW_CasambiVersionWarning(11, undef, undef), 'ok', 'missing range -> ok');
