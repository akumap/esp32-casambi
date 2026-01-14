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

// Device Name
#define DEVICE_NAME               "ESP32 Casambi"

// ============================================================================
// STORAGE PATHS
// ============================================================================

#define CONFIG_FILE_PATH          "/casambi_config.json"
#define WIFI_FILE_PATH            "/wifi_config.json"

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

extern bool debugEnabled;  // Debug output toggle (defined in main.cpp)

#endif // CONFIG_H
