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
#define MIN_PROTOCOL_VERSION      10
#define MAX_PROTOCOL_VERSION      10

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

// Watchdog timeout (seconds) - hardware WDT
#define WDT_TIMEOUT_SECONDS             30

// Maximum consecutive BLE reconnect failures before ESP restart
#define MAX_RECONNECT_FAILURES          10

// ============================================================================
// WEBSERVER / WEBSOCKET SETTINGS
// ============================================================================

// Maximum simultaneous WebSocket clients. New connections beyond this limit are
// rejected (closed immediately on connect), so existing clients — notably the
// FHEM gateway link — are never evicted by connection churn. Bounds memory use
// under load. Realistic need is ~2 (FHEM + one browser); 3 leaves headroom.
#define WS_MAX_CLIENTS                  3

// Depth of the inter-task broadcast queue (stores String* pointers).
// Broadcasts from the BLE task are enqueued here and drained by loop(), so
// _ws->textAll() is always called from the loop task — never from a BLE task
// callback — which avoids races with the async_tcp task's _clients management.
// If the queue is full the broadcast is silently dropped (lighting state will
// catch up on the next BLE notification anyway).
#define WS_BROADCAST_QUEUE_DEPTH        8

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

// ============================================================================
// GLOBAL FLAGS
// ============================================================================

extern bool bleDebugEnabled;      // BLE/crypto verbose debug (defined in main.cpp)
extern bool casambiDebugEnabled;  // Casambi network events: unit states, echo, callbacks (defined in main.cpp)
extern bool webDebugEnabled;      // Web API request logging (defined in main.cpp)
extern bool parseDebugEnabled;    // Protocol parse compact output (defined in main.cpp)
extern bool heapDebugEnabled;     // Heap monitoring output (defined in main.cpp)

#endif // CONFIG_H
