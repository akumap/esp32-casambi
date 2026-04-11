package main;
use JSON;
use MIME::Base64;

# ============================================================================
# CasambiGW — FHEM gateway module for the ESP32 Casambi BLE bridge
#
# Manages the persistent WebSocket connection to the ESP32 firmware.
# On each (re-)connect the ESP32 sends a "hello" message with the full
# unit snapshot.  CasambiGW compares this against the existing CasambiUnit
# devices and reacts as follows:
#
#   • State updates (brightness, on/off, colorTemp, vertical, online)
#     and capability updates for already-known units are applied
#     IMMEDIATELY and automatically.
#
#   • Structural changes (new units, units removed from the Casambi
#     network) are stored as PENDING CHANGES.  The readings "syncState"
#     and "pendingSync" describe what was detected.  The user then
#     decides whether to adopt the changes:
#
#       set <name> applyChanges    → create new / delete removed devices
#       set <name> discardChanges  → clear the pending list, do nothing
#
# Define syntax:
#   define <name> CasambiGW <ip>[:<port>]
#
# Example:
#   define MeinCasambi CasambiGW 192.168.178.111
#   define MeinCasambi CasambiGW 192.168.178.111:80
#
# Attributes:
#   autocreate          0|1  Create new CasambiUnit devices on applyChanges
#                            (default: 1)
#   deleteRemovedUnits  0|1  Delete FHEM device when applyChanges is called
#                            and unit is gone from Casambi network (default: 1)
#                            When 0: device stays but "online" is set to false
# ============================================================================

use constant WS_PING_INTERVAL    => 30;   # seconds between WS keepalive pings
use constant WS_PONG_TIMEOUT     => 60;   # seconds without pong → reconnect
use constant MIN_FIRMWARE_BUILD  => 1;    # minimum accepted ESP32 build number

# ============================================================================
# Module registration
# ============================================================================

sub CasambiGW_Initialize {
    my $hash = shift;
    $hash->{DefFn}    = "CasambiGW_Define";
    $hash->{UndefFn}  = "CasambiGW_Undefine";
    $hash->{SetFn}    = "CasambiGW_Set";
    $hash->{ReadFn}   = "CasambiGW_Read";
    $hash->{ReadyFn}  = "CasambiGW_Ready";
    $hash->{AttrList} = "autocreate:0,1 deleteRemovedUnits:0,1 "
                      . $readingFnAttributes;
    return undef;
}

# ============================================================================
# Device lifecycle
# ============================================================================

sub CasambiGW_Define {
    my ($hash, $def) = @_;
    my @args = split /\s+/, $def;
    return "Usage: define <name> CasambiGW <ip>[:<port>]" if @args < 3;

    my $addr = $args[2];
    $addr .= ":80" unless $addr =~ /:/;

    $hash->{DeviceName} = $addr;
    $hash->{GW_IP}      = (split /:/, $addr)[0];
    $hash->{GW_PORT}    = (split /:/, $addr)[1] // 80;
    $hash->{wsState}    = "disconnected";
    $hash->{buf}        = "";
    $hash->{UNIT_BY_ID} = {};

    DevIo_CloseDev($hash);
    DevIo_OpenDev($hash, 0, "CasambiGW_WsHandshake");
    return undef;
}

sub CasambiGW_Undefine {
    my ($hash, $name) = @_;
    RemoveInternalTimer($hash, "CasambiGW_Ping");
    DevIo_CloseDev($hash);
    return undef;
}

# ============================================================================
# SetFn — gateway control commands
# ============================================================================

sub CasambiGW_Set {
    my ($hash, $name, $cmd, @args) = @_;

    if ($cmd eq "?") {
        return "Unknown argument $cmd, choose one of "
             . "applyChanges:noArg discardChanges:noArg";
    }

    if ($cmd eq "applyChanges") {
        return CasambiGW_ApplyPendingChanges($hash);
    }

    if ($cmd eq "discardChanges") {
        delete $hash->{PENDING_CHANGES};
        readingsBeginUpdate($hash);
        readingsBulkUpdate($hash, "syncState",   "ok");
        readingsBulkUpdate($hash, "pendingSync", "none");
        readingsEndUpdate($hash, 1);
        Log3 $name, 3, "$name: Pending changes discarded";
        return undef;
    }

    return "Unknown command '$cmd', choose one of applyChanges:noArg discardChanges:noArg";
}

# ============================================================================
# WebSocket handshake (called by DevIo after TCP connect)
# ============================================================================

sub CasambiGW_WsHandshake {
    my $hash = shift;
    my $name = $hash->{NAME};

    $hash->{buf}     = "";
    $hash->{wsState} = "handshake";

    my $key = encode_base64(pack("C16", map { int(rand(256)) } 1..16), "");
    $hash->{wsKey} = $key;

    my $host = $hash->{GW_IP};
    my $req  = "GET /ws HTTP/1.1\r\n"
             . "Host: $host\r\n"
             . "Upgrade: websocket\r\n"
             . "Connection: Upgrade\r\n"
             . "Sec-WebSocket-Key: $key\r\n"
             . "Sec-WebSocket-Version: 13\r\n"
             . "\r\n";
    DevIo_SimpleWrite($hash, $req, 0);
    Log3 $name, 4, "$name: WebSocket handshake sent";
    return undef;
}

# ============================================================================
# ReadFn — called by FHEM's select loop when data arrives
# ============================================================================

sub CasambiGW_Read {
    my $hash = shift;
    my $name = $hash->{NAME};

    my $data = DevIo_SimpleRead($hash);
    return undef unless defined $data;

    $hash->{buf} .= $data;

    if ($hash->{wsState} eq "handshake") {
        return undef unless $hash->{buf} =~ /\r\n\r\n/;

        if ($hash->{buf} =~ /HTTP\/1\.[01] 101/) {
            $hash->{buf} =~ s/^.*?\r\n\r\n//s;
            $hash->{wsState} = "connected";
            $hash->{lastPong} = gettimeofday();
            RemoveInternalTimer($hash, "CasambiGW_Ping");
            InternalTimer(gettimeofday() + WS_PING_INTERVAL, "CasambiGW_Ping", $hash);
            readingsSingleUpdate($hash, "state", "connected", 1);
            Log3 $name, 3, "$name: WebSocket connected";
            CasambiGW_ProcessWsFrames($hash) if length($hash->{buf}) > 0;
        } else {
            my $status = (split /\r\n/, $hash->{buf})[0];
            Log3 $name, 2, "$name: WebSocket handshake failed: $status";
            $hash->{wsState} = "disconnected";
            RemoveInternalTimer($hash, "CasambiGW_Ping");
            DevIo_Disconnected($hash);
        }
        return undef;
    }

    CasambiGW_ProcessWsFrames($hash) if $hash->{wsState} eq "connected";
    return undef;
}

# ============================================================================
# WebSocket frame parser
# ============================================================================

sub CasambiGW_ProcessWsFrames {
    my $hash = shift;
    my $name = $hash->{NAME};

    while (length($hash->{buf}) >= 2) {
        my ($byte0, $byte1) = unpack("CC", $hash->{buf});
        my $opcode = $byte0 & 0x0F;
        my $paylen = $byte1 & 0x7F;    # server→client frames are never masked
        my $hdrLen = 2;

        if ($paylen == 126) {
            last if length($hash->{buf}) < 4;
            $paylen = unpack("n", substr($hash->{buf}, 2, 2));
            $hdrLen = 4;
        } elsif ($paylen == 127) {
            last if length($hash->{buf}) < 10;
            $paylen = unpack("N", substr($hash->{buf}, 6, 4));
            $hdrLen = 10;
        }

        last if length($hash->{buf}) < $hdrLen + $paylen;

        my $payload = substr($hash->{buf}, $hdrLen, $paylen);
        $hash->{buf} = substr($hash->{buf}, $hdrLen + $paylen);

        if ($opcode == 0x08) {
            Log3 $name, 3, "$name: WebSocket close frame received";
            $hash->{wsState} = "disconnected";
            RemoveInternalTimer($hash, "CasambiGW_Ping");
            DevIo_Disconnected($hash);
            last;
        } elsif ($opcode == 0x09) {
            _CasambiGW_WsSendPong($hash, $payload);
        } elsif ($opcode == 0x0A) {
            $hash->{lastPong} = gettimeofday();
            Log3 $name, 5, "$name: WS pong received";
        } elsif ($opcode == 0x01 || $opcode == 0x00) {
            CasambiGW_HandleWsMessage($hash, $payload);
        }
    }
}

# ============================================================================
# Keepalive ping/pong
# ============================================================================

sub CasambiGW_Ping {
    my $hash = shift;
    my $name = $hash->{NAME};

    return unless $hash->{wsState} eq "connected";

    if (gettimeofday() - ($hash->{lastPong} // 0) > WS_PONG_TIMEOUT) {
        Log3 $name, 2, "$name: WebSocket pong timeout — reconnecting";
        $hash->{wsState} = "disconnected";
        readingsSingleUpdate($hash, "state", "disconnected", 1);
        DevIo_Disconnected($hash);
        return;
    }

    my @mask = map { int(rand(256)) } 1..4;
    DevIo_SimpleWrite($hash, pack("CC", 0x89, 0x80) . pack("CCCC", @mask), 0);
    Log3 $name, 5, "$name: WS ping sent";

    InternalTimer(gettimeofday() + WS_PING_INTERVAL, "CasambiGW_Ping", $hash);
}

sub _CasambiGW_WsSendPong {
    my ($hash, $payload) = @_;
    my $len   = length($payload);
    my @mask  = map { int(rand(256)) } 1..4;
    my $mdata = join("", map {
        chr(ord(substr($payload, $_, 1)) ^ $mask[$_ % 4])
    } 0 .. $len - 1);
    my $frame = pack("CC", 0x8A, 0x80 | $len) . pack("CCCC", @mask) . $mdata;
    DevIo_SimpleWrite($hash, $frame, 0);
}

# ============================================================================
# Message dispatcher
# ============================================================================

sub CasambiGW_HandleWsMessage {
    my ($hash, $json) = @_;
    my $name = $hash->{NAME};

    my $msg;
    eval { $msg = decode_json($json); };
    if ($@ || !$msg) {
        Log3 $name, 2, "$name: WS JSON parse error: $@";
        return;
    }

    my $type = $msg->{type} // "";

    if ($type eq "hello") {
        my $n = ref($msg->{units}) eq "ARRAY" ? scalar @{$msg->{units}} : 0;
        Log3 $name, 3, "$name: WS hello ($n units)";
        CasambiGW_HandleHello($hash, $msg);
    } elsif ($type eq "unit_state") {
        Log3 $name, 5, "$name: WS unit_state id=$msg->{id}";
        CasambiGW_RouteUnitState($hash, $msg);
    } elsif ($type eq "connection_state") {
        my $ble = $msg->{connected} ? "ble_connected" : "ble_disconnected";
        readingsSingleUpdate($hash, "ble_state", $ble, 1);
        Log3 $name, 3, "$name: BLE state: $ble";
    }
}

# ============================================================================
# Hello handler — compare snapshot against existing devices, detect changes
# ============================================================================

sub CasambiGW_HandleHello {
    my ($hash, $msg) = @_;
    my $name = $hash->{NAME};

    # Check ESP32 firmware build number
    my $build = $msg->{build} // 0;
    readingsSingleUpdate($hash, "esp32Build", $build, 1);
    if ($build < MIN_FIRMWARE_BUILD) {
        Log3 $name, 2, "$name: WARNING: ESP32 build $build < minimum " . MIN_FIRMWARE_BUILD
                     . " — please update the ESP32 firmware";
        readingsSingleUpdate($hash, "esp32BuildWarning",
            "ESP32 build $build < minimum " . MIN_FIRMWARE_BUILD, 1);
    } else {
        readingsSingleUpdate($hash, "esp32BuildWarning", "ok", 1);
    }

    # Build MAC→FHEM-name registry from all CasambiUnit devices of this GW
    my %byMac;
    for my $devName (sort keys %defs) {
        my $dh = $defs{$devName};
        next unless ($dh->{TYPE}    // "") eq "CasambiUnit";
        next unless ($dh->{GW_NAME} // "") eq $name;
        my $mac = AttrVal($devName, "casambiMac", "");
        $byMac{$mac} = $devName if $mac;
    }

    my (%newById, %seenMac);
    my (@pendingNew, %pendingRemove);

    for my $unit (@{$msg->{units}}) {
        my $mac = $unit->{address} // "";
        $seenMac{$mac} = 1 if $mac;

        my $devName = $byMac{$mac} // "";

        if (!$devName || !$defs{$devName}) {
            # Unknown unit — queue for manual creation
            push @pendingNew, $unit;
            Log3 $name, 3, "$name: New unit detected: '$unit->{name}' (MAC $mac) — run 'set $name applyChanges'";
            next;
        }

        my $dh = $defs{$devName};

        # Known unit: immediately apply capability and state updates.
        # Capability updates are idempotent (no-op when nothing changed).
        CasambiUnit_SetCapabilities($dh, $unit);
        CasambiUnit_UpdateFromState($dh, $unit);

        $newById{ $unit->{id} } = $devName if defined $unit->{id};
    }

    # Detect units that disappeared from the Casambi network
    for my $mac (keys %byMac) {
        next if $seenMac{$mac};
        my $devName = $byMac{$mac};
        next unless $defs{$devName};
        $pendingRemove{$mac} = $devName;
        Log3 $name, 3, "$name: Unit gone from network: '$devName' (MAC $mac) — run 'set $name applyChanges'";
    }

    # Store/update pending changes and update status readings
    if (@pendingNew || %pendingRemove) {
        $hash->{PENDING_CHANGES} = {
            newUnits    => \@pendingNew,
            removedMacs => \%pendingRemove,
        };

        my @parts;
        if (@pendingNew) {
            my $names = join(", ", map { $_->{name} // "?" } @pendingNew);
            push @parts, scalar(@pendingNew) . " new ($names)";
        }
        if (%pendingRemove) {
            my $names = join(", ", values %pendingRemove);
            push @parts, scalar(keys %pendingRemove) . " removed ($names)";
        }
        my $summary = join("; ", @parts);

        readingsBeginUpdate($hash);
        readingsBulkUpdate($hash, "syncState",   "changes_pending");
        readingsBulkUpdate($hash, "pendingSync", $summary);
        readingsEndUpdate($hash, 1);

        Log3 $name, 2, "$name: Pending changes: $summary";
    } else {
        # No structural changes — clear any stale pending state
        delete $hash->{PENDING_CHANGES};
        readingsBeginUpdate($hash);
        readingsBulkUpdate($hash, "syncState",   "ok");
        readingsBulkUpdate($hash, "pendingSync", "none");
        readingsEndUpdate($hash, 1);
    }

    # Update unit-ID map (covers known units; new ones added after applyChanges)
    $hash->{UNIT_BY_ID} = \%newById;

    readingsSingleUpdate($hash, "lastSync", FmtDateTime(gettimeofday()), 1);
    Log3 $name, 3, "$name: Sync complete ("
        . scalar(keys %newById) . " known, "
        . scalar(@pendingNew)   . " pending new, "
        . scalar(keys %pendingRemove) . " pending removed)";
}

# ============================================================================
# Apply pending structural changes (triggered by "set <gw> applyChanges")
# ============================================================================

sub CasambiGW_ApplyPendingChanges {
    my $hash    = shift;
    my $name    = $hash->{NAME};
    my $pending = $hash->{PENDING_CHANGES};

    unless ($pending) {
        Log3 $name, 3, "$name: applyChanges — no pending changes";
        return undef;
    }

    # --- Create new units ---
    if (AttrVal($name, "autocreate", 1)) {
        for my $unit (@{$pending->{newUnits}}) {
            my $devName = CasambiGW_CreateUnit($hash, $unit);
            next unless $devName && $defs{$devName};
            CasambiUnit_SetCapabilities($defs{$devName}, $unit);
            CasambiUnit_UpdateFromState($defs{$devName},  $unit);
            # Register in id map for live routing
            $hash->{UNIT_BY_ID}{ $unit->{id} } = $devName if defined $unit->{id};
        }
    } else {
        Log3 $name, 3, "$name: applyChanges — autocreate=0, skipping "
            . scalar(@{$pending->{newUnits}}) . " new unit(s)";
    }

    # --- Handle removed units ---
    for my $mac (keys %{$pending->{removedMacs}}) {
        my $devName = $pending->{removedMacs}{$mac};
        next unless $defs{$devName};
        if (AttrVal($name, "deleteRemovedUnits", 1)) {
            Log3 $name, 3, "$name: Deleting '$devName' (MAC $mac — removed from Casambi network)";
            # Also delete companion vertical device if it exists
            my $vName = "${devName}_vertical";
            fhem("delete $vName") if $defs{$vName};
            fhem("delete $devName");
        } else {
            readingsSingleUpdate($defs{$devName}, "online", "false", 1);
            Log3 $name, 3, "$name: '$devName' (MAC $mac) marked offline (removed from network, deleteRemovedUnits=0)";
        }
    }

    delete $hash->{PENDING_CHANGES};
    readingsBeginUpdate($hash);
    readingsBulkUpdate($hash, "syncState",   "ok");
    readingsBulkUpdate($hash, "pendingSync", "none");
    readingsEndUpdate($hash, 1);

    Log3 $name, 3, "$name: Pending changes applied";
    return undef;
}

# ============================================================================
# unit_state router — fast path via cached UNIT_BY_ID map
# ============================================================================

sub CasambiGW_RouteUnitState {
    my ($hash, $msg) = @_;
    my $name   = $hash->{NAME};
    my $unitId = $msg->{id} // return;

    my $devName = $hash->{UNIT_BY_ID}{$unitId};

    unless ($devName && $defs{$devName}) {
        # Not in map — rebuild once (e.g. after FHEM restart before first hello)
        Log3 $name, 4, "$name: unit_state id=$unitId not in map — rebuilding";
        my %newById;
        for my $dn (sort keys %defs) {
            my $dh = $defs{$dn};
            next unless ($dh->{TYPE}    // "") eq "CasambiUnit";
            next unless ($dh->{GW_NAME} // "") eq $name;
            my $id = ReadingsVal($dn, "casambiId", "");
            $newById{$id} = $dn if $id ne "";
        }
        $hash->{UNIT_BY_ID} = \%newById;
        $devName = $newById{$unitId};
    }

    return unless $devName && $defs{$devName};
    CasambiUnit_UpdateFromState($defs{$devName}, $msg);
}

# ============================================================================
# Auto-create a new CasambiUnit device
# ============================================================================

sub CasambiGW_CreateUnit {
    my ($hash, $unit) = @_;
    my $name = $hash->{NAME};
    my $mac  = $unit->{address} // "";

    # Derive a clean FHEM device name from the Casambi unit name
    my $raw     = $unit->{name} // ("Unit_" . ($unit->{id} // "?"));
    my $devName = "Casambi_$raw";
    $devName =~ s/\s+/_/g;
    $devName =~ s/[^a-zA-Z0-9_\-\.]//g;

    # Ensure uniqueness
    my $base = $devName;
    my $i    = 2;
    while ($defs{$devName}) { $devName = "${base}_$i"; $i++; }

    Log3 $name, 3, "$name: Creating CasambiUnit '$devName' for '$raw' (MAC $mac)";
    fhem("define $devName CasambiUnit $name $mac");

    return $defs{$devName} ? $devName : undef;
}

# ============================================================================
# Command forwarding — called by CasambiUnit's SetFn
# ============================================================================

sub CasambiGW_SendCommand {
    my ($gwName, $unitId, $cmd, $value) = @_;
    my $gwHash = $defs{$gwName};
    return unless $gwHash;

    my $ip   = $gwHash->{GW_IP};
    my $port = $gwHash->{GW_PORT} // 80;
    my ($url, $json);

    if ($cmd eq "on") {
        $url  = "http://$ip:$port/api/units/$unitId/level";
        $json = '{"level":255}';
    } elsif ($cmd eq "off") {
        $url  = "http://$ip:$port/api/units/$unitId/level";
        $json = '{"level":0}';
    } elsif ($cmd eq "brightness") {
        my $level = int(($value // 0) * 2.55);
        $level = 255 if $level > 255;
        $url  = "http://$ip:$port/api/units/$unitId/level";
        $json = "{\"level\":$level}";
    } elsif ($cmd eq "colorTemp") {
        $url  = "http://$ip:$port/api/units/$unitId/temperature";
        $json = "{\"kelvin\":$value}";
    } elsif ($cmd eq "vertical") {
        $url  = "http://$ip:$port/api/units/$unitId/vertical";
        $json = "{\"vertical\":$value}";
    } else {
        return;
    }

    HttpUtils_NonblockingGet({
        url      => $url,
        timeout  => 5,
        method   => "POST",
        header   => "Content-Type: application/json",
        data     => $json,
        callback => sub {
            my ($param, $err, $data) = @_;
            Log 2, "CasambiGW: HTTP error (unit $unitId $cmd): $err" if $err;
        }
    });
}

# ============================================================================
# ReadyFn — reconnect when disconnected
# ============================================================================

sub CasambiGW_Ready {
    my $hash = shift;
    return DevIo_OpenDev($hash, 1, "CasambiGW_WsHandshake");
}

1;

=pod
=item device
=item summary FHEM gateway for ESP32 Casambi BLE bridge (WebSocket connection)
=item summary_DE FHEM-Gateway für ESP32-Casambi-BLE-Brücke (WebSocket-Verbindung)
=begin html

<a name="CasambiGW"></a>
<h3>CasambiGW</h3>
<ul>
  Manages the WebSocket connection to the ESP32 Casambi BLE gateway firmware.
  On each (re-)connect the ESP32 sends a full unit snapshot ("hello").
  State and capability updates for already-known units are applied immediately.
  Structural changes (new or removed units) are held as <em>pending changes</em>
  and must be adopted explicitly via <code>set &lt;name&gt; applyChanges</code>.
  <br><br>
  <b>Define</b><br>
  <code>define &lt;name&gt; CasambiGW &lt;ip&gt;[:<port>]</code>
  <br><br>
  <b>Set commands</b>
  <ul>
    <li><b>applyChanges</b> &mdash; create new and/or delete removed
        <a href="#CasambiUnit">CasambiUnit</a> devices as listed in
        <em>pendingSync</em></li>
    <li><b>discardChanges</b> &mdash; clear the pending list without making
        any structural changes</li>
  </ul>
  <br>
  <b>Attributes</b>
  <ul>
    <li><b>autocreate</b> 0|1 &mdash; whether applyChanges creates new
        CasambiUnit devices (default: 1)</li>
    <li><b>deleteRemovedUnits</b> 0|1 &mdash; whether applyChanges deletes
        the FHEM device (1) or only sets <em>online</em> to false (0)
        for units gone from the Casambi network (default: 1)</li>
  </ul>
  <br>
  <b>Readings</b>
  <ul>
    <li><b>state</b> &mdash; WebSocket connection state
        (connected / disconnected)</li>
    <li><b>ble_state</b> &mdash; ESP32 BLE link state
        (ble_connected / ble_disconnected)</li>
    <li><b>syncState</b> &mdash; ok | changes_pending</li>
    <li><b>pendingSync</b> &mdash; human-readable summary of pending changes,
        e.g. "2 new (Kueche, Bad); 1 removed (Casambi_Old)"</li>
    <li><b>lastSync</b> &mdash; timestamp of the last hello sync</li>
    <li><b>esp32Build</b> &mdash; build number reported by the ESP32 in the hello message</li>
    <li><b>esp32BuildWarning</b> &mdash; "ok" or a human-readable warning when the ESP32
        build is below the minimum required build</li>
  </ul>
</ul>
=end html
=cut
