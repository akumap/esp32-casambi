package main;
use JSON;
use MIME::Base64;

# ============================================================================
# Globals
# ============================================================================

my $_updatingStatus = 0;
my %_debouncePending;

# Casambi unit ID → FHEM device name
my %_deviceMap = (
    1  => "Occhio_AZ_Stehleuchte",
    2  => "Occhio_Piu_Spuele",
    3  => "Occhio_Kueche",
    5  => "Occhio_Peter_Nachttisch",
    7  => "Occhio_Mito_Sospeso",
    8  => "Occhio_Mel_Luna",
    9  => "Occhio_AZ_Decke",
    10 => "Occhio_AZ_Schrank",
    11 => "Occhio_IFC",
);

# ============================================================================
# FHEM module registration
# ============================================================================

sub OcchioControl_Initialize {
    my $hash = shift;
    $hash->{DefFn}   = "OcchioControl_Define";
    $hash->{UndefFn} = "OcchioControl_Undefine";
    $hash->{ReadFn}  = "OcchioControl_Read";
    $hash->{ReadyFn} = "OcchioControl_Ready";
}

# ============================================================================
# Device lifecycle
# ============================================================================

sub OcchioControl_Define {
    my ($hash, $def) = @_;
    $hash->{DeviceName} = "192.168.178.111:80";
    $hash->{wsState}    = "disconnected";
    $hash->{buf}        = "";
    DevIo_CloseDev($hash);
    return DevIo_OpenDev($hash, 0, "OcchioControl_WsHandshake");
}

sub OcchioControl_Undefine {
    my ($hash, $name) = @_;
    DevIo_CloseDev($hash);
    return undef;
}

# ============================================================================
# WebSocket connection setup
# ============================================================================

sub OcchioControl_WsHandshake {
    my $hash = shift;
    my $name = $hash->{NAME};

    $hash->{buf}     = "";
    $hash->{wsState} = "handshake";

    # Random 16-byte key, base64-encoded (no trailing newline)
    my $key = encode_base64(pack("C16", map { int(rand(256)) } 1..16), "");
    $hash->{wsKey} = $key;

    my $host = "192.168.178.111";
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
# ReadFn — called by FHEM's select loop when data is available
# ============================================================================

sub OcchioControl_Read {
    my $hash = shift;
    my $name = $hash->{NAME};

    my $data = DevIo_SimpleRead($hash);
    return undef unless defined $data;

    $hash->{buf} .= $data;

    if ($hash->{wsState} eq "handshake") {
        # Wait until we have a complete HTTP response header block
        return undef unless $hash->{buf} =~ /\r\n\r\n/;

        if ($hash->{buf} =~ /HTTP\/1\.[01] 101/) {
            # Strip HTTP headers, keep any trailing WebSocket data
            $hash->{buf} =~ s/^.*?\r\n\r\n//s;
            $hash->{wsState} = "connected";
            readingsSingleUpdate($hash, "state", "connected", 1);
            Log3 $name, 3, "$name: WebSocket connected";
            OcchioControl_ProcessWsFrames($hash) if length($hash->{buf}) > 0;
        } else {
            my $status = (split /\r\n/, $hash->{buf})[0];
            Log3 $name, 2, "$name: WebSocket handshake failed: $status";
            $hash->{wsState} = "disconnected";
            DevIo_Disconnected($hash);
        }
        return undef;
    }

    if ($hash->{wsState} eq "connected") {
        OcchioControl_ProcessWsFrames($hash);
    }
    return undef;
}

# ============================================================================
# WebSocket frame parser
# ============================================================================

sub OcchioControl_ProcessWsFrames {
    my $hash = shift;
    my $name = $hash->{NAME};

    while (length($hash->{buf}) >= 2) {
        my ($byte0, $byte1) = unpack("CC", $hash->{buf});
        my $opcode = $byte0 & 0x0F;
        my $paylen = $byte1 & 0x7F;  # server→client frames are never masked
        my $hdrLen = 2;

        if ($paylen == 126) {
            last if length($hash->{buf}) < 4;
            $paylen = unpack("n", substr($hash->{buf}, 2, 2));
            $hdrLen = 4;
        } elsif ($paylen == 127) {
            last if length($hash->{buf}) < 10;
            # Ignore high 32 bits; payload > 4 GB won't occur here
            $paylen = unpack("N", substr($hash->{buf}, 6, 4));
            $hdrLen = 10;
        }

        last if length($hash->{buf}) < $hdrLen + $paylen;

        my $payload = substr($hash->{buf}, $hdrLen, $paylen);
        $hash->{buf} = substr($hash->{buf}, $hdrLen + $paylen);

        if ($opcode == 0x08) {
            # Close frame — server requested close
            Log3 $name, 3, "$name: WebSocket close frame received";
            $hash->{wsState} = "disconnected";
            DevIo_Disconnected($hash);
            last;
        } elsif ($opcode == 0x09) {
            # Ping — respond with pong
            OcchioControl_WsSendPong($hash, $payload);
        } elsif ($opcode == 0x01 || $opcode == 0x00) {
            # Text frame (0x01) or continuation (0x00)
            OcchioControl_HandleWsMessage($hash, $payload);
        }
        # 0x0A pong: ignore
    }
}

# Send a masked pong frame (client→server frames must be masked per RFC 6455)
sub OcchioControl_WsSendPong {
    my ($hash, $payload) = @_;
    my $len   = length($payload);
    my @mask  = map { int(rand(256)) } 1..4;
    my $mdata = $payload;
    for my $i (0 .. $len - 1) {
        substr($mdata, $i, 1) = chr(ord(substr($payload, $i, 1)) ^ $mask[$i % 4]);
    }
    my $frame = pack("CC", 0x8A, 0x80 | $len) . pack("CCCC", @mask) . $mdata;
    DevIo_SimpleWrite($hash, $frame, 0);
}

# ============================================================================
# Message dispatcher
# ============================================================================

sub OcchioControl_HandleWsMessage {
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
        Log3 $name, 4, "$name: WS hello ($n units)";
        for my $unit (@{$msg->{units}}) {
            OcchioControl_UpdateFromUnitState($unit);
        }
    } elsif ($type eq "unit_state") {
        Log3 $name, 5, "$name: WS unit_state id=$msg->{id} level=$msg->{level}";
        OcchioControl_UpdateFromUnitState($msg);
    } elsif ($type eq "connection_state") {
        my $ble = $msg->{connected} ? "ble_connected" : "ble_disconnected";
        readingsSingleUpdate($hash, "ble_state", $ble, 1);
        Log3 $name, 3, "$name: BLE state: $ble";
    }
}

# ============================================================================
# Apply a unit state object (from hello or unit_state) to FHEM readings.
# Used by both WebSocket push and the existing REST polling path.
# ============================================================================

sub OcchioControl_UpdateFromUnitState {
    my $unit = shift;

    my $devName = $_deviceMap{ $unit->{id} };
    return unless $devName;

    my $dev_hash = $defs{$devName};
    unless ($dev_hash) {
        Log 2, "OcchioControl: Device '$devName' nicht in FHEM gefunden!";
        return;
    }

    my $level      = $unit->{level} // 0;
    my $on         = defined($unit->{on}) ? $unit->{on} : ($level > 0);
    my $online     = $unit->{online} ? "true" : "false";
    my $state      = $on ? "on" : "off";
    my $brightness = int($level / 2.55 + 0.5);
    $brightness    = 100 if $brightness > 100;

    readingsBeginUpdate($dev_hash);
    readingsBulkUpdate($dev_hash, "online",     $online);
    readingsBulkUpdate($dev_hash, "state",      $state);
    readingsBulkUpdate($dev_hash, "brightness", $brightness);

    if (defined $unit->{vertical}) {
        readingsBulkUpdate($dev_hash, "vertical", $unit->{vertical});
    }

    if (defined $unit->{colorTemp} && defined $unit->{cctMin} && defined $unit->{cctMax}) {
        my $raw = $unit->{colorTemp};
        my $min = $unit->{cctMin};
        my $max = $unit->{cctMax};
        if ($max > $min) {
            # raw 0-255 is linearly scaled: 0 → cctMin, 255 → cctMax
            my $kelvin = int($min + ($raw / 255.0) * ($max - $min) + 0.5);
            readingsBulkUpdate($dev_hash, "colorTemp", $kelvin);
        }
    }

    $_updatingStatus = 1;
    readingsEndUpdate($dev_hash, 1);
    $_updatingStatus = 0;

    Log 4, "OcchioControl: Updated $devName (state=$state, brightness=$brightness)";
}

# ============================================================================
# ReadyFn — called by FHEM when device is disconnected, for reconnection
# ============================================================================

sub OcchioControl_Ready {
    my $hash = shift;
    return DevIo_OpenDev($hash, 1, "OcchioControl_WsHandshake");
}

# ============================================================================
# Command forwarding: Notify + SendCommand (unchanged)
# ============================================================================

sub OcchioControl_Notify {
    my ($device, $event) = @_;

    return if $_updatingStatus;

    Log 1, "OcchioControl: Device=$device Event='$event'";

    # "undefined XX" Events von Homebridge behandeln
    if ($event =~ /^undefined\s+(.*)/) {
        my $value = int($1);
        if ($value <= 100) {
            $event = "brightness $value";
            Log 1, "OcchioControl: Converted to Event='$event' (Brightness)";
        } else {
            # Gerätespezifische Grenzen aus Attributen, Fallback auf Casambi-typische Werte
            my $cctMin = AttrVal($device, "cctMin", 2700);
            my $cctMax = AttrVal($device, "cctMax", 4000);
            my $kelvin = int(1000000 / $value);
            $kelvin = $cctMin if $kelvin < $cctMin;
            $kelvin = $cctMax if $kelvin > $cctMax;
            $event = "colorTemp $kelvin";
            Log 1, "OcchioControl: Converted to Event='$event' (ColorTemp from $value Mired)";
        }
    }

    my $unit_id = AttrVal($device, "casambiUnitId", "");
    return if (!$unit_id);

    # on/off sofort senden
    if ($event =~ /^(on|off)$/) {
        OcchioControl_SendCommand($device, $unit_id, $event);
        return;
    }

    # Analoge Typen: debounced senden
    my $type;
    if    ($event =~ /^brightness\s+/) { $type = "brightness"; }
    elsif ($event =~ /^colorTemp\s+/)  { $type = "colorTemp";  }
    elsif ($event =~ /^vertical\s+/)   { $type = "vertical";   }
    else  { return; }

    my $key = "${device}_${type}";
    $_debouncePending{$key} = { device => $device, unit_id => $unit_id, event => $event };

    RemoveInternalTimer("debounce_${key}");
    InternalTimer(
        gettimeofday() + 0.3,
        sub {
            my $pending = delete $_debouncePending{$key};
            return unless $pending;
            OcchioControl_SendCommand($pending->{device}, $pending->{unit_id}, $pending->{event});
        },
        "debounce_${key}"
    );
}

sub OcchioControl_SendCommand {
    my ($device, $unit_id, $event) = @_;
    my $esp32_ip = "192.168.178.111";
    my ($url, $json);

    if ($event eq "on") {
        $url  = "http://$esp32_ip/api/units/$unit_id/level";
        $json = '{"level":255}';
        fhem("setreading $device state on");
        # fhem("setreading $device brightness 100");
    }
    elsif ($event eq "off") {
        $url  = "http://$esp32_ip/api/units/$unit_id/level";
        $json = '{"level":0}';
        fhem("setreading $device state off");
        # fhem("setreading $device brightness 0");
    }
    elsif ($event =~ /^brightness\s+(.*)/) {
        my $brightness = int($1);
        my $level = int($brightness * 2.55);
        $url  = "http://$esp32_ip/api/units/$unit_id/level";
        $json = '{"level":'.$level.'}';
        fhem("setreading $device brightness $brightness");
        # fhem("setreading $device state " . ($brightness > 0 ? "on" : "off"));
    }
    elsif ($event =~ /^colorTemp\s+(.*)/) {
        my $value = int($1);
        my $kelvin = ($value < 500) ? int(1000000 / $value) : $value;

        # Gerätespezifische Grenzen aus Attributen, Fallback auf Casambi-typische Werte
        my $cctMin = AttrVal($device, "cctMin", 2700);
        my $cctMax = AttrVal($device, "cctMax", 4000);
        $kelvin = $cctMin if $kelvin < $cctMin;
        $kelvin = $cctMax if $kelvin > $cctMax;

        my $mired = int(1000000 / $kelvin);
        $url  = "http://$esp32_ip/api/units/$unit_id/temperature";
        $json = '{"kelvin":'.$kelvin.'}';
        fhem("setreading $device colorTemp $mired");
    }
    elsif ($event =~ /^vertical\s+(.*)/) {
        my $vertical = int($1);
        $vertical = 0   if $vertical < 0;
        $vertical = 255 if $vertical > 255;
        $url  = "http://$esp32_ip/api/units/$unit_id/vertical";
        $json = '{"vertical":'.$vertical.'}';
        fhem("setreading $device vertical $vertical");
    }
    else {
        return;
    }

    Log3 $device, 3, "$device: Casambi -> Unit $unit_id: $event";

    HttpUtils_NonblockingGet({
        url      => $url,
        timeout  => 5,
        method   => "POST",
        header   => "Content-Type: application/json",
        data     => $json,
        callback => sub {
            my ($param, $err, $data) = @_;
            Log3 $device, 2, "$device: Casambi error: $err" if $err;
        }
    });
}

# ============================================================================
# Fallback REST polling (called externally, e.g. via FHEM at-timer)
# Uses OcchioControl_UpdateFromUnitState for consistent conversion.
# ============================================================================

sub OcchioControl_UpdateStatus {
    my $esp32_ip = "192.168.178.111";

    HttpUtils_NonblockingGet({
        url     => "http://$esp32_ip/api/units",
        timeout => 5,
        callback => sub {
            my ($param, $err, $data) = @_;
            if ($err) {
                Log 2, "OcchioControl: UpdateStatus HTTP error: $err";
                return;
            }

            my $decoded;
            eval { $decoded = decode_json($data); };
            if ($@ || !$decoded) {
                Log 2, "OcchioControl: UpdateStatus JSON error: $@";
                return;
            }

            for my $unit (@{$decoded->{units}}) {
                OcchioControl_UpdateFromUnitState($unit);
            }
        }
    });
}

1;
