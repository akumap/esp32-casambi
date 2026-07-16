/**
 * ESP32 Casambi Configuration
 *
 * Constants and configuration for Casambi BLE protocol
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// CASAMBI PROTOCOL CONSTANTS
// ============================================================================

// BLE Service and Characteristics
#define CASAMBI_SERVICE_UUID      "0000fe4d-0000-1000-8000-00805f9b34fb"
#define CASAMBI_AUTH_CHAR_UUID    "c9ffde48-ca5a-0001-ab83-8f519b482f77"

// Manufacturer ID
#define CASAMBI_MFG_ID            0x03C3

// Protocol Version
// Range of Casambi BLE protocol versions the firmware is known to work with.
// v11 is in active use by current networks; the on-air handling is
// version-tolerant (mismatches are logged, not enforced — see
// checkCasambiVersions() and CasambiClient), so a config whose protocolVersion
// falls outside this range is accepted with a warning rather than rejected.
#define MIN_PROTOCOL_VERSION      10
#define MAX_PROTOCOL_VERSION      11

// ESP32 firmware build number.
// Normally injected at compile time by scripts/build_number.py
// (git rev-list --count origin/main).  The fallback below is used only
// when building outside of PlatformIO (e.g. Arduino IDE) or without git.
#ifndef FIRMWARE_BUILD
#define FIRMWARE_BUILD            0
#endif

// Minimum Casambi unit firmware version (the numeric part of "Evolution/X.Y")
#define MIN_UNIT_FIRMWARE_VERSION 48.0f

// Device Name
#define DEVICE_NAME               "ESP32 Casambi"

// ============================================================================
// STORAGE PATHS
// ============================================================================

#define CONFIG_FILE_PATH          "/casambi_config.json"
#define WIFI_FILE_PATH            "/wifi_config.json"
// Temp/backup companions for atomic, crash-safe writes. New data is written to
// *.tmp, validated, then the live file is renamed to *.bak and the temp moved
// into place; the backup is kept until the new file is confirmed, and is used
// for recovery at load time if the live file is missing or corrupt.
#define CONFIG_TMP_PATH           "/casambi_config.json.tmp"
#define CONFIG_BAK_PATH           "/casambi_config.json.bak"
#define WIFI_TMP_PATH             "/wifi_config.json.tmp"
#define WIFI_BAK_PATH             "/wifi_config.json.bak"
// Marker file: when present at boot, the firmware re-reads the Casambi cloud
// configuration before BLE/web are started, then reboots into normal operation.
#define REFRESH_FLAG_PATH         "/refresh_pending"

// ============================================================================
// EVENT LOG
// ============================================================================

// LittleFS ring-buffer files (ping-pong). Each is filled, then we switch to
// the other and clear it — so the most recent 1–2 files of history survive.
#define LOG_DIR                   "/log"
#define LOG_FILE_A                "/log/log_a.bin"
#define LOG_FILE_B                "/log/log_b.bin"
#define LOG_META_FILE             "/log/log_meta.bin"

// Maximum size of each ring-buffer file (bytes). Two files → up to 2× history.
#define LOG_FILE_MAX_SIZE         16384

// Maximum message length stored per entry (excluding NUL).
#define LOG_MSG_MAX               120

// Number of "last words" entries kept in RTC NOINIT RAM. These survive
// WDT/panic/SW resets (not power-off) and are flushed to LittleFS on next boot.
// Each LogEntry is 132 bytes → 28 × 132 = 3696 bytes (< 4 KB RTC budget).
#define LOG_RTC_CAPACITY          28

// Default NTP server (configurable at runtime via 'ntp set' / POST /api/ntp).
#define NTP_SERVER_DEFAULT        "pool.ntp.org"

// ============================================================================
// CRYPTO CONSTANTS
// ============================================================================

// AES Key size
#define AES_KEY_SIZE              16

// CMAC size
#define CMAC_SIZE                 16

// Nonce size
#define NONCE_SIZE                16

// ECDH Public key coordinate size (SECP256R1)
#define ECDH_KEY_SIZE             32

// ============================================================================
// WEB API AUTHENTICATION
// ============================================================================

// HTTP header carrying the API token on every protected REST request and on
// the WebSocket upgrade.
#define API_KEY_HEADER            "X-API-Key"

// Domain-separation prefix mixed into the token derivation so the value on the
// wire is NOT the raw Casambi cloud password:
//   apiToken = hex( SHA-256( API_TOKEN_PREFIX || casambiPassword ) )
// FHEM derives the same token from the password the user configures. Auth is
// only enforced when a Casambi password is stored (empty password = open, e.g.
// for old configs predating this field).
#define API_TOKEN_PREFIX          "casambi-api:"

// ============================================================================
// TIMEOUTS
// ============================================================================

#define WIFI_CONNECT_TIMEOUT_MS   10000
#define BLE_CONNECT_TIMEOUT_MS    10000
#define API_REQUEST_TIMEOUT_MS    15000

// ============================================================================
// RECONNECT & WATCHDOG SETTINGS
// ============================================================================

// BLE auto-reconnect interval (ms) - wait before trying to reconnect
#define BLE_RECONNECT_INTERVAL_MS       5000

// BLE reconnect maximum backoff (ms)
#define BLE_RECONNECT_MAX_BACKOFF_MS    60000

// WiFi reconnect check interval (ms)
#define WIFI_RECONNECT_INTERVAL_MS      30000

// Connection health check interval (ms) - verify BLE is still alive
#define CONNECTION_CHECK_INTERVAL_MS    10000

// Heap monitoring interval (ms) - check free heap periodically
#define HEAP_MONITOR_INTERVAL_MS        15000

// Minimum free heap before forced restart (bytes)
#define HEAP_CRITICAL_THRESHOLD         20000

// Consecutive low-heap readings required before a protective restart. A single
// transient dip (e.g. TCP/WS buffers during a web request, a BLE reconnect)
// recovers on its own; only a sustained shortfall warrants a reboot.
#define HEAP_CRITICAL_CONSECUTIVE       3

// Watchdog timeout (seconds) - hardware WDT. Must stay ABOVE the longest
// blocking BLE operation on the loop task: a GATT read against a half-dead
// link (keepalive) can block for NimBLE's full 30 s ATT procedure timeout, so
// 45 s leaves deterministic margin instead of racing that timeout.
#define WDT_TIMEOUT_SECONDS             45

// Send the keepalive GATT read only after this much notification silence.
// While 0x06/0x0C traffic is flowing the link is demonstrably alive and the
// extra ATT round-trip (with its up-to-30 s timeout, see WDT above) is
// pointless radio traffic and mutex contention.
#define BLE_KEEPALIVE_IDLE_MS           60000

// Maximum consecutive INTERNAL BLE failures (auth, key exchange, GATT
// structure) before ESP restart. Plain link-loss/timeout failures — the peer
// is simply absent, e.g. lights cut by a wall switch — never trigger a
// restart: rebooting cannot help there, and the reconnect loop keeps retrying
// at max backoff indefinitely instead.
#define MAX_RECONNECT_FAILURES          10

// --- RSSI quality gate on connect ("gateway re-roll") -----------------------
// All units of a Casambi network advertise the SAME virtual address, so the
// physical unit serving a session is whichever advertisement the ESP32 happens
// to catch first — a lottery. If the settled link RSSI after a successful
// connect is below this threshold (dBm), the link is dropped and re-connected
// to re-roll onto a closer unit (e.g. an always-powered actor next to the
// ESP32). Set to 0 to disable the gate.
#define BLE_MIN_CONNECT_RSSI            (-85)

// Maximum re-rolls per connect() call. After that the LAST roll is accepted
// regardless of RSSI (a previous, better unit cannot be re-targeted — every
// re-connect is a fresh lottery), so connectivity always wins over quality.
#define BLE_RSSI_REROLL_MAX             2

// Settle time before the gate reads the RSSI. Readings taken immediately
// after the connect can be far off (unaveraged controller value).
#define BLE_RSSI_SETTLE_MS              2000

// ============================================================================
// WEBSERVER / WEBSOCKET SETTINGS
// ============================================================================

// Maximum simultaneous WebSocket clients. loop() trims the client list to this
// many via cleanupClients(); excess/stale clients are closed. Bounds memory use
// under connection churn. Realistic need is ~2 (FHEM + one browser); 3 leaves
// headroom. (A connect-time hard reject was tried but leaked client structures
// under churn — see git history — so the cap is enforced via cleanupClients.)
#define WS_MAX_CLIENTS                  3

// Depth of the inter-task broadcast queue (stores String* pointers).
// Broadcasts from the BLE task are enqueued here and drained by loop(), so
// _ws->textAll() is always called from the loop task — never from a BLE task
// callback — which avoids races with the async_tcp task's _clients management.
// Sized above the worst case a single 0x06 packet can produce (~40 unit
// records at MTU 247, one broadcast each) so group/scene bursts are never
// dropped; the queue itself is only pointers (64 × 4 B). Should it still
// overflow, the drop is flagged and loop() pushes a fresh hello snapshot so
// clients cannot stay stale on a missed unit_state.
#define WS_BROADCAST_QUEUE_DEPTH        64

// Depth of the REST→loop BLE command queue (stores BleCommand values).
// Control handlers on the async_tcp task only validate and enqueue; the loop
// task dequeues one command per iteration and performs the BLE operation, so
// the up-to-1 s BLE mutex wait / GATT write never blocks the async_tcp task.
// A full queue answers 503 (client retries) — 8 in-flight commands is far
// beyond what FHEM or a UI produces between two loop iterations (~10 ms).
#define BLE_CMD_QUEUE_DEPTH             8

// ============================================================================
// BLE PACKET CONSTANTS
// ============================================================================

// Packet header length
#define PACKET_HEADER_LEN         4

// Operation lifetime default
#define OPERATION_LIFETIME        5

// ============================================================================
// API ENDPOINTS
// ============================================================================

#define CASAMBI_API_BASE          "https://api.casambi.com"
#define API_NETWORK_UUID_PATH     "/network/uuid/"
#define API_NETWORK_SESSION_PATH  "/network/"
#define API_NETWORK_CONFIG_PATH   "/network/"

// Cloud TLS: the requests above carry the Casambi network password and the
// session token, so the HTTPS server certificate is validated against the
// Mozilla root-CA bundle embedded in the arduino-esp32 core (see
// api_client.cpp). This is the default and requires no configuration here.
//
// Escape hatch: building with -DCASAMBI_TLS_INSECURE disables that validation
// (restores the old, MITM-exposed behaviour) for core builds that do not export
// the bundle symbol. It is logged loudly at runtime and should not be used in
// production.

// ============================================================================
// GLOBAL FLAGS
// ============================================================================

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Guards the NetworkConfig String fields that can change at runtime
// (ntpServer, autoConnectAddress, casambiPassword). All writers run on the
// loop task; the async_tcp task copies these strings under this mutex before
// use, so a concurrent String reassignment cannot free the buffer mid-read.
// The bool/uint8 state fields are NOT guarded (torn reads are impossible,
// momentary inconsistency is acceptable). Defined in main.cpp.
extern SemaphoreHandle_t g_configMutex;

extern bool bleDebugEnabled;      // BLE/crypto verbose debug (defined in main.cpp)
extern bool casambiDebugEnabled;  // Casambi network events: unit states, echo, callbacks (defined in main.cpp)
extern bool webDebugEnabled;      // Web API request logging (defined in main.cpp)
extern bool parseDebugEnabled;    // Protocol parse compact output (defined in main.cpp)
extern bool heapDebugEnabled;     // Heap monitoring output (defined in main.cpp)

#endif // CONFIG_H
