package main;
use MIME::Base64;
use Digest::SHA qw(sha256_hex);

# JSON is a hard requirement, but a plain `use JSON;` would abort the module
# load with a bare compile error on systems without the Perl module. Load it
# when the file body runs (FHEM `do`es the file) and report a readable error
# from Define instead. (No BEGIN block: a file-scope `my` re-executed at
# runtime would clear a value assigned during compilation.)
my $CasambiGW_missingJSON = "";
eval { require JSON; JSON->import(); 1 }
    or $CasambiGW_missingJSON = "Perl module JSON not installed "
                              . "(e.g. 'apt install libjson-perl' or 'cpanm JSON')";

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
#   casambiPassword          Casambi network password; the X-API-Key token is
#                            derived from it. Required once the ESP32 has API
#                            authentication enabled. Stored in plaintext in
#                            fhem.cfg — prefer `set <gw> password <pw>`, which
#                            keeps it in FHEM's obfuscated key-value store.
# ============================================================================

use constant WS_PING_INTERVAL    => 30;   # seconds between WS keepalive pings
use constant WS_PONG_TIMEOUT     => 60;   # seconds without pong → reconnect
use constant MIN_FIRMWARE_BUILD  => 1;    # minimum accepted ESP32 build number
use constant INFO_POLL_SETUP     => 15;   # seconds between /api/info polls while the ESP is in setup mode
use constant INFO_POLL_OFFLINE   => 30;   # seconds between /api/info polls while the ESP is unreachable

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
    $hash->{AttrList} = "autocreate:0,1 deleteRemovedUnits:0,1 casambiPassword "
                      . $readingFnAttributes;
    return undef;
}

# ============================================================================
# API authentication
#
# The ESP32 protects its REST/WebSocket API once a Casambi network password is
# stored. The token sent on every request is NOT the raw password but a derived
# value that the ESP computes the same way:
#   apiToken = hex( SHA-256( "casambi-api:" . <password> ) )
# Configure the password via:  set <gw> password <casambi-network-pw>
# (stored in FHEM's obfuscated key-value store) — or, legacy, via the
# plaintext attribute casambiPassword. Returns "" when no password is set
# (then the ESP must also be running without auth, e.g. a config predating
# this feature).
# ============================================================================

sub CasambiGW_PasswordKey {
    my $hash = shift;
    return "CasambiGW_" . $hash->{NAME} . "_password";
}

sub CasambiGW_ApiToken {
    my $hash = shift;
    # Prefer the key store (set <gw> password ...); fall back to the legacy
    # plaintext attribute for existing installations.
    my ($err, $pw) = getKeyValue(CasambiGW_PasswordKey($hash));
    $pw = AttrVal($hash->{NAME}, "casambiPassword", "")
        if $err || !defined $pw || $pw eq "";
    return "" if !defined $pw || $pw eq "";
    return sha256_hex("casambi-api:" . $pw);
}

# Convert a brightness percentage (0..100) to a Casambi level byte (0..255).
# Uses rounded integer arithmetic so the endpoints are exact: 0 % → 0 and
# 100 % → 255. The naive int($pct * 2.55) truncates 100 * 2.55 (≈ 254.9999 in
# floating point) down to 254, which would never reach full brightness and would
# disagree with the explicit `on` command (255). The input is clamped to 0..100
# first so out-of-range values cannot produce a byte outside 0..255.
sub CasambiGW_PercentToByte {
    my ($pct) = @_;
    $pct = 0   if !defined $pct || $pct < 0;
    $pct = 100 if $pct > 100;
    return int($pct * 255 / 100 + 0.5);
}

# Classify the outcome of the refreshCasambi POST from HttpUtils' ($err, $code).
# Returns one of: 'reboot' (expected connection drop, no HTTP status),
# 'accepted' (2xx), 'auth' (401/403), 'conflict' (409), 'gatewayerror' (5xx),
# 'unexpected' (any other status). Kept as a pure helper so the mapping can be
# unit-tested without a live HttpUtils call. A non-2xx response must never be
# reported as accepted.
sub CasambiGW_ClassifyRefreshResponse {
    my ($err, $code) = @_;
    $code //= 0;
    return 'reboot'       if $err && !$code;    # dropped before any HTTP status
    return 'accepted'     if $code >= 200 && $code < 300;
    return 'auth'         if $code == 401 || $code == 403;
    return 'conflict'     if $code == 409;
    return 'gatewayerror' if $code >= 500;
    return 'unexpected';
}

# Build an HTTP header string for HttpUtils, appending the X-API-Key line when a
# token is configured. $base may be undef/empty.
sub CasambiGW_AuthHeader {
    my ($hash, $base) = @_;
    my $token = CasambiGW_ApiToken($hash);
    my @lines;
    push @lines, $base if defined $base && $base ne "";
    push @lines, "X-API-Key: $token" if $token ne "";
    return join("\r\n", @lines);
}

# ============================================================================
# Device lifecycle
# ============================================================================

sub CasambiGW_Define {
    my ($hash, $def) = @_;
    return "CasambiGW: $CasambiGW_missingJSON" if $CasambiGW_missingJSON;
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
    $hash->{infoPolling}= 0;

    # Do not open the WebSocket blindly: first check /api/info and only connect
    # once the ESP32 reports configured:true (see CasambiGW_CheckInfo).
    readingsSingleUpdate($hash, "state", "initializing", 1);
    CasambiGW_StartInfoPoll($hash);
    return undef;
}

sub CasambiGW_Undefine {
    my ($hash, $name) = @_;
    RemoveInternalTimer($hash, "CasambiGW_Ping");
    RemoveInternalTimer($hash, "CasambiGW_Poll");
    DevIo_CloseDev($hash);
    return undef;
}

# ============================================================================
# Connection bring-up gated by GET /api/info
#
# Before opening the WebSocket the module queries /api/info. Only when the
# ESP32 reports configured:true do we connect; while it is still in setup mode
# (configured:false) or unreachable we poll periodically and connect
# automatically once it becomes ready (e.g. after finishing the setup portal).
# ============================================================================

sub CasambiGW_StartInfoPoll {
    my ($hash, $delay) = @_;
    $delay //= 1;
    $hash->{infoPolling} = 1;
    $hash->{wsState}     = "disconnected";
    RemoveInternalTimer($hash, "CasambiGW_Ping");
    RemoveInternalTimer($hash, "CasambiGW_Poll");
    DevIo_CloseDev($hash);                      # also removes us from readyfnlist
    InternalTimer(gettimeofday() + $delay, "CasambiGW_Poll", $hash);
    return undef;
}

sub CasambiGW_Poll {
    my $hash = shift;
    return if ($hash->{wsState} // "") eq "connected";
    CasambiGW_CheckInfo($hash);
    return undef;
}

sub CasambiGW_CheckInfo {
    my $hash = shift;
    my $url  = "http://$hash->{GW_IP}:$hash->{GW_PORT}/api/info";
    HttpUtils_NonblockingGet({
        url      => $url,
        timeout  => 5,
        hash     => $hash,
        callback => \&CasambiGW_InfoCb,
    });
    return undef;
}

sub CasambiGW_InfoCb {
    my ($param, $err, $data) = @_;
    my $hash = $param->{hash};
    my $name = $hash->{NAME};

    # Stale callback: a `set reconnect` or successful WS connect may have ended
    # the polling episode while this request was in flight. Only the active
    # polling state machine may act on /api/info results.
    return undef unless $hash->{infoPolling};

    # De-duplicate poll chains: StartInfoPoll cannot cancel an in-flight HTTP
    # request, so without this a reconnect during a pending poll would leave
    # two timer chains polling in parallel forever.
    RemoveInternalTimer($hash, "CasambiGW_Poll");

    if ($err || !defined($data) || $data eq "") {
        Log3 $name, 4, "$name: /api/info unreachable: " . ($err // "no data");
        readingsSingleUpdate($hash, "state", "unreachable", 1);
        InternalTimer(gettimeofday() + INFO_POLL_OFFLINE, "CasambiGW_Poll", $hash);
        return undef;
    }

    my $info;
    eval { $info = decode_json($data); };
    if ($@ || ref($info) ne "HASH") {
        Log3 $name, 2, "$name: /api/info bad JSON: " . ($@ // "");
        readingsSingleUpdate($hash, "state", "unreachable", 1);
        InternalTimer(gettimeofday() + INFO_POLL_OFFLINE, "CasambiGW_Poll", $hash);
        return undef;
    }

    my $configured = $info->{configured} ? 1 : 0;
    readingsBeginUpdate($hash);
    readingsBulkUpdate($hash, "esp32Build", $info->{build})  if defined $info->{build};
    readingsBulkUpdate($hash, "configured", $configured ? "true" : "false");
    readingsBulkUpdate($hash, "network", $info->{network})   if defined $info->{network};
    readingsEndUpdate($hash, 1);

    if ($configured) {
        Log3 $name, 3, "$name: ESP configured (network '"
            . ($info->{network} // "?") . "') - connecting WebSocket";
        $hash->{infoPolling} = 0;
        RemoveInternalTimer($hash, "CasambiGW_Poll");
        DevIo_CloseDev($hash);
        DevIo_OpenDev($hash, 0, "CasambiGW_WsHandshake");
    } else {
        Log3 $name, 3, "$name: ESP still in setup mode - waiting for provisioning";
        readingsSingleUpdate($hash, "state", "setup_required", 1);
        InternalTimer(gettimeofday() + INFO_POLL_SETUP, "CasambiGW_Poll", $hash);
    }
    return undef;
}

# ============================================================================
# SetFn — gateway control commands
# ============================================================================

sub CasambiGW_Set {
    my ($hash, $name, $cmd, @args) = @_;

    if ($cmd eq "?") {
        return "Unknown argument $cmd, choose one of "
             . "applyChanges:noArg discardChanges:noArg reconnect:noArg "
             . "refreshCasambi:noArg password";
    }

    if ($cmd eq "reconnect") {
        Log3 $name, 3, "$name: reconnect requested";
        CasambiGW_StartInfoPoll($hash);   # re-check /api/info, then connect
        return undef;
    }

    if ($cmd eq "password") {
        # Store the Casambi network password in FHEM's obfuscated key-value
        # store (instead of the plaintext casambiPassword attribute). Without
        # an argument the stored password is removed.
        my $key = CasambiGW_PasswordKey($hash);
        if (@args && defined $args[0] && $args[0] ne "") {
            my $err = setKeyValue($key, $args[0]);
            return "Failed to store password: $err" if $err;
            Log3 $name, 3, "$name: Casambi password stored (key store) — reconnecting";
        } else {
            setKeyValue($key, undef);
            Log3 $name, 3, "$name: stored Casambi password removed — reconnecting";
        }
        # Reconnect so the next handshake uses the new token.
        CasambiGW_StartInfoPoll($hash);
        return undef;
    }

    if ($cmd eq "refreshCasambi") {
        return CasambiGW_RefreshCasambi($hash);
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

    return "Unknown command '$cmd', choose one of applyChanges:noArg "
         . "discardChanges:noArg reconnect:noArg refreshCasambi:noArg password";
}

# ============================================================================
# refreshCasambi — ask the ESP32 to re-read its configuration from the Casambi
# cloud using the stored network password. The ESP frees BLE, downloads the
# fresh config and reboots; afterwards our ReadyFn/InfoPoll reconnects the
# WebSocket and CasambiGW_Read re-syncs the CasambiUnit devices (adding/updating
# them). Because the ESP reboots right after accepting the request, a dropped
# connection here is expected and not treated as an error.
# ============================================================================

sub CasambiGW_RefreshCasambi {
    my $hash = shift;
    my $name = $hash->{NAME};

    my $ip   = $hash->{GW_IP};
    my $port = $hash->{GW_PORT} // 80;
    return "No gateway IP known yet — wait until the ESP32 is connected"
        unless defined $ip && $ip ne "";

    my $url = "http://$ip:$port/api/refreshCasambi";
    Log3 $name, 3, "$name: refreshCasambi requested → POST $url";

    HttpUtils_NonblockingGet({
        url      => $url,
        timeout  => 10,
        method   => "POST",
        header   => CasambiGW_AuthHeader($hash, "Content-Type: application/json"),
        data     => "{}",
        hash     => $hash,
        callback => sub {
            my ($param, $err, $data) = @_;
            my $h = $param->{hash};
            my $n = $h->{NAME};
            my $code = $param->{code} // 0;
            my $class = CasambiGW_ClassifyRefreshResponse($err, $code);
            # A transport error with no HTTP status is the expected case: the ESP
            # reboots immediately after *accepting* the request, so the TCP
            # connection is reset before it can answer. Every other class means an
            # HTTP status was received; a non-2xx status is never "accepted".
            if ($class eq 'reboot') {
                Log3 $n, 4,
                    "$n: refreshCasambi — connection dropped after request "
                  . "(expected, ESP is rebooting): $err";
            } elsif ($class eq 'accepted') {
                Log3 $n, 3, "$n: refreshCasambi accepted by ESP (HTTP $code)";
            } elsif ($class eq 'auth') {
                Log3 $n, 2,
                    "$n: refreshCasambi rejected — authentication failed "
                  . "(HTTP $code); check the casambiPassword / API token";
            } elsif ($class eq 'conflict') {
                Log3 $n, 3,
                    "$n: refreshCasambi not started — a refresh is already "
                  . "in progress (HTTP 409)";
            } elsif ($class eq 'gatewayerror') {
                Log3 $n, 2, "$n: refreshCasambi failed — gateway error (HTTP $code)";
            } else {
                Log3 $n, 2,
                    "$n: refreshCasambi unexpected response (HTTP $code): $data";
            }
        }
    });

    return undef;
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

    my $host  = $hash->{GW_IP};
    my $token = CasambiGW_ApiToken($hash);
    my $auth  = $token ne "" ? "X-API-Key: $token\r\n" : "";
    my $req  = "GET /ws HTTP/1.1\r\n"
             . "Host: $host\r\n"
             . "Upgrade: websocket\r\n"
             . "Connection: Upgrade\r\n"
             . "Sec-WebSocket-Key: $key\r\n"
             . "Sec-WebSocket-Version: 13\r\n"
             . $auth
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
    unless (defined $data) {
        # DevIo has already called DevIo_Disconnected; mirror it in our own
        # state so the ping timer stops treating the link as alive.
        $hash->{wsState} = "disconnected";
        return undef;
    }

    $hash->{buf} .= $data;

    if ($hash->{wsState} eq "handshake") {
        return undef unless $hash->{buf} =~ /\r\n\r\n/;

        if ($hash->{buf} =~ /HTTP\/1\.[01] 101/) {
            $hash->{buf} =~ s/^.*?\r\n\r\n//s;
            $hash->{wsState} = "connected";
            $hash->{wsFailCount} = 0;
            $hash->{lastPong} = gettimeofday();
            RemoveInternalTimer($hash, "CasambiGW_Ping");
            InternalTimer(gettimeofday() + WS_PING_INTERVAL, "CasambiGW_Ping", $hash);
            readingsSingleUpdate($hash, "state", "connected", 1);
            Log3 $name, 3, "$name: WebSocket connected";
            CasambiGW_ProcessWsFrames($hash) if length($hash->{buf}) > 0;
        } else {
            my $status = (split /\r\n/, $hash->{buf})[0] // "";
            # First failure of an episode logs prominently (with an auth hint
            # on 401); repeats — one per backoff interval — go to level 4 so a
            # persistent condition does not flood the log.
            $hash->{wsFailCount} = ($hash->{wsFailCount} // 0) + 1;
            Log3 $name, ($hash->{wsFailCount} == 1 ? 2 : 4),
                "$name: WebSocket handshake failed: $status";
            if ($status =~ /\s401\s/ && $hash->{wsFailCount} == 1) {
                Log3 $name, 2, "$name: ESP32 rejected the API token (401) — set the "
                             . "Casambi network password: 'set $name password <pw>' "
                             . "(or attr $name casambiPassword)";
            }
            # A failed upgrade means auth failure or the ESP is back in setup
            # mode / not ready. Back off to the offline poll interval instead
            # of hammering /api/info + WebSocket every second.
            CasambiGW_StartInfoPoll($hash, INFO_POLL_OFFLINE);
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

    # Known limitation: the FIN bit is not evaluated — a fragmented text
    # message would be handed to the JSON parser in pieces. The ESP32
    # (AsyncWebSocket textAll) always sends unfragmented frames, so no
    # reassembly is needed against this server.
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
    my $len = length($payload);
    # RFC 6455 §5.5: control frames must not exceed 125 bytes.
    # A ping with a larger payload is a protocol violation; drop it silently.
    if ($len > 125) {
        Log3 $hash->{NAME}, 2, "$hash->{NAME}: WS ping payload too large ($len bytes), dropping pong";
        return;
    }
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
        CasambiGW_UpdateGateway($hash, $msg->{gateway});
        Log3 $name, 3, "$name: BLE state: $ble";
    }
}

# ============================================================================
# Gateway transparency — which Casambi unit currently serves as BLE gateway.
# Fed from the ESP32 "hello" and "connection_state" messages. Useful now and
# as a basis for the planned gateway-hopping feature.
# ============================================================================

sub CasambiGW_UpdateGateway {
    my ($hash, $gw) = @_;
    return unless ref($gw) eq "HASH";

    my $connected = $gw->{connected} ? 1 : 0;
    readingsBeginUpdate($hash);
    readingsBulkUpdate($hash, "gatewayState", $connected ? "connected" : "disconnected");
    readingsBulkUpdate($hash, "gatewayMac",   $gw->{mac}  // "");
    readingsBulkUpdate($hash, "gatewayName",  $gw->{name} // "");
    readingsEndUpdate($hash, 1);
    return undef;
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

    # Network name travels in the (authenticated) hello — the unauthenticated
    # /api/info deliberately does not expose it.
    readingsSingleUpdate($hash, "network", $msg->{network}, 1)
        if defined $msg->{network} && $msg->{network} ne "";

    if ($build < MIN_FIRMWARE_BUILD) {
        Log3 $name, 2, "$name: WARNING: ESP32 build $build < minimum " . MIN_FIRMWARE_BUILD
                     . " — please update the ESP32 firmware";
        readingsSingleUpdate($hash, "esp32BuildWarning",
            "ESP32 build $build < minimum " . MIN_FIRMWARE_BUILD, 1);
    } else {
        readingsSingleUpdate($hash, "esp32BuildWarning", "ok", 1);
    }

    # Report the currently used gateway (name/MAC/status) for transparency.
    CasambiGW_UpdateGateway($hash, $msg->{gateway});

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

    my $structuralChanges = 0;

    # --- Create new units ---
    if (AttrVal($name, "autocreate", 1)) {
        for my $unit (@{$pending->{newUnits}}) {
            my $devName = CasambiGW_CreateUnit($hash, $unit);
            next unless $devName && $defs{$devName};
            $structuralChanges++;
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
            $structuralChanges++;
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
    # Created/deleted devices live only in memory until the config is saved.
    Log3 $name, 2, "$name: device definitions changed — run 'save' to persist them"
        if $structuralChanges;
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

    # Known-unknown id (exists in the map as undef): a pending/discarded unit
    # that keeps pushing state. Skip silently instead of re-scanning %defs on
    # every push; the cache is replaced by the next hello.
    return if exists $hash->{UNIT_BY_ID}{$unitId} && !defined $devName;

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
        unless ($devName && $defs{$devName}) {
            # Still unknown — negative-cache the miss (cleared by next hello).
            $hash->{UNIT_BY_ID}{$unitId} = undef;
            return;
        }
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

    # Validate the MAC before interpolating it into a fhem() command. The
    # address comes from the network config / hello message; rejecting anything
    # that is not a plain colon-separated MAC prevents command injection via a
    # crafted address field.
    if ($mac !~ /^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/i) {
        Log3 $name, 2,
            "$name: refusing to create unit '$raw' — invalid MAC address '$mac'";
        return undef;
    }

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
        my $level = CasambiGW_PercentToByte($value);
        $url  = "http://$ip:$port/api/units/$unitId/level";
        $json = "{\"level\":$level}";
    } elsif ($cmd eq "colorTemp") {
        $url  = "http://$ip:$port/api/units/$unitId/temperature";
        $json = "{\"kelvin\":$value}";
    } elsif ($cmd eq "vertical") {
        $url  = "http://$ip:$port/api/units/$unitId/vertical";
        $json = "{\"value\":$value}";
    } else {
        return;
    }

    HttpUtils_NonblockingGet({
        url      => $url,
        timeout  => 5,
        method   => "POST",
        header   => CasambiGW_AuthHeader($gwHash, "Content-Type: application/json"),
        data     => $json,
        callback => sub {
            my ($param, $err, $data) = @_;
            if ($err) {
                Log3 $gwName, 2, "$gwName: HTTP error (unit $unitId $cmd): $err";
                return;
            }
            # Application-level rejections (503 BLE down, 401 auth, 404 unknown
            # unit) are not transport errors — without this check the command
            # vanished silently while the FHEM reading optimistically showed
            # the new state.
            my $code = $param->{code} // 0;
            if ($code != 200) {
                my $hint = $code == 401
                    ? " — check 'set $gwName password' / attr casambiPassword" : "";
                Log3 $gwName, 3, "$gwName: unit $unitId $cmd rejected: HTTP $code "
                               . (defined $data ? $data : "") . $hint;
            }
        }
    });
}

# ============================================================================
# ReadyFn — reconnect when disconnected
# ============================================================================

sub CasambiGW_Ready {
    my $hash = shift;
    # While we are polling /api/info, reconnection is driven by that timer,
    # not by DevIo's auto-reconnect.
    return undef if $hash->{infoPolling};
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
    <li><b>reconnect</b> &mdash; re-check <code>/api/info</code> and re-open the
        WebSocket connection to the ESP32</li>
    <li><b>refreshCasambi</b> &mdash; ask the ESP32 to re-read its configuration
        from the Casambi cloud using the stored network password. The ESP
        downloads the fresh config and reboots; afterwards the module reconnects
        automatically and new/changed units appear as <em>pending changes</em>
        (adopt them with <code>applyChanges</code>). Requires that the ESP has a
        stored Casambi password (set once via the serial <code>refresh</code>
        command or initial setup).</li>
    <li><b>password</b> &lt;casambi-network-password&gt; &mdash; store the
        Casambi network password in FHEM's obfuscated key-value store and
        reconnect. Preferred over the plaintext <em>casambiPassword</em>
        attribute; the module derives the <code>X-API-Key</code> token from it
        (SHA-256 of "casambi-api:" + password) for REST calls and the
        WebSocket handshake. Without an argument the stored password is
        removed.</li>
  </ul>
  <br>
  <b>Attributes</b>
  <ul>
    <li><b>autocreate</b> 0|1 &mdash; whether applyChanges creates new
        CasambiUnit devices (default: 1)</li>
    <li><b>deleteRemovedUnits</b> 0|1 &mdash; whether applyChanges deletes
        the FHEM device (1) or only sets <em>online</em> to false (0)
        for units gone from the Casambi network (default: 1)</li>
    <li><b>casambiPassword</b> &mdash; legacy plaintext alternative to
        <code>set &lt;name&gt; password</code> (which takes precedence when
        both are set). Kept for existing installations; note that attribute
        values are stored in plaintext in fhem.cfg.</li>
  </ul>
  <br>
  <b>Readings</b>
  <ul>
    <li><b>state</b> &mdash; connection state: initializing / setup_required
        (ESP still in setup portal) / unreachable / connected / disconnected.
        The module polls <code>/api/info</code> and only opens the WebSocket
        once the ESP reports <em>configured:true</em>, reconnecting
        automatically after the setup portal has been completed.</li>
    <li><b>configured</b> &mdash; whether the ESP32 has a valid configuration
        (true) or is still in setup mode (false), from <code>/api/info</code></li>
    <li><b>network</b> &mdash; Casambi network name, taken from the
        authenticated WebSocket <em>hello</em> message (the unauthenticated
        <code>/api/info</code> deliberately does not expose it)</li>
    <li><b>gatewayState</b> &mdash; BLE gateway link: connected / disconnected</li>
    <li><b>gatewayName</b> &mdash; name of the Casambi unit currently acting as
        the BLE gateway (empty while disconnected)</li>
    <li><b>gatewayMac</b> &mdash; BLE MAC of the current gateway unit
        (empty while disconnected). Together with gatewayName this provides
        transparency over which unit the ESP32 is connected through &mdash;
        also a basis for the planned gateway-hopping feature.</li>
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
