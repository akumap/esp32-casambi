/**
 * Casambi Network Configuration Structures
 *
 * Data structures for network configuration, units, groups, scenes, and keys
 */

#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <Arduino.h>
#include <vector>
#include "../config.h"

// ============================================================================
// ENCRYPTION KEY
// ============================================================================

struct CasambiKey {
    uint8_t id;
    uint8_t type;
    uint8_t role;
    String name;
    uint8_t key[AES_KEY_SIZE];

    CasambiKey() : id(0), type(0), role(0), name("") {
        memset(key, 0, AES_KEY_SIZE);
    }
};

// ============================================================================
// NETWORK DEVICES
// ============================================================================

// One controllable channel of a unit, taken verbatim from the cloud fixture
// definition (GET /fixture/{type}). `typeName` is the raw control type as the
// cloud names it, lower-cased ("dimmer", "vertical", "temperature", "white",
// "rgb", "xy", "slider", ...) — the authoritative source for field / reading
// names all the way into FHEM. offset/length are in BITS; for the common
// byte-aligned 8-bit control the state-byte index is offset/8. `value` is the
// current decoded raw value (0 .. 2^length-1); for "temperature" min/max carry
// the Kelvin bounds so consumers can present Kelvin without extra lookups.
struct UnitControl {
    String   typeName;
    uint8_t  offset;
    uint8_t  length;
    uint16_t min;
    uint16_t max;
    uint16_t value;

    UnitControl() : typeName(""), offset(0), length(0), min(0), max(0), value(0) {}
};

struct CasambiUnit {
    uint8_t deviceId;
    uint16_t type;
    String uuid;
    String address;
    String name;
    String firmware;

    // Authoritative capabilities from the cloud fixture (/fixture/{type}).
    // `controls` drives the generic naming and decoding; the flags below are
    // derived from it (kept for the send path and backward compatibility).
    std::vector<UnitControl> controls;
    uint8_t stateLength;     // state-blob length in bytes (from the fixture)
    bool hasFixture;         // true once the fixture definition has been applied

    // Capabilities (derived from `controls`, or from the mode-string heuristic
    // as a fallback when the fixture could not be fetched).
    uint8_t numChannels;     // 1=dimmer only, 2=dimmer+aux, 3=dimmer+vertical+temp
    bool hasCCT;             // Has color temperature control
    bool hasVertical;        // Has vertical light distribution control
    uint16_t cctMinKelvin;   // Minimum color temperature (Kelvin)
    uint16_t cctMaxKelvin;   // Maximum color temperature (Kelvin)

    // Current state (updated from 0x06 packets)
    bool online;
    bool on;
    uint8_t level;           // Current brightness 0-255
    uint8_t vertical;        // Current vertical value 0-255 (if hasVertical)
    uint8_t colorTemp;       // Current color temp 0-255 normalized (if hasCCT)

    CasambiUnit() : deviceId(0), type(0), uuid(""), address(""),
                    name(""), firmware(""),
                    controls(), stateLength(0), hasFixture(false),
                    numChannels(1), hasCCT(false), hasVertical(false),
                    cctMinKelvin(0), cctMaxKelvin(0),
                    online(false), on(false), level(0),
                    vertical(127), colorTemp(0) {}

    // The control descriptor for a given cloud type name (case-insensitive),
    // or nullptr. Lets the decode/API map a state byte to its named control.
    const UnitControl* controlByType(const char* type) const {
        for (const auto& c : controls) {
            if (c.typeName.equalsIgnoreCase(type)) return &c;
        }
        return nullptr;
    }
};

// Stable, unique name of a unit's control for the generic API: the sole
// control of a cloud type keeps the plain type name ("temperature"), a type
// that occurs more than once gets a 0-based index in fixture order ("dimmer0",
// "dimmer1" — e.g. dual-dimmer Uplight/Downlight fixtures like Oligo Grace).
// Derived on demand (not stored) so configs persisted by older firmware get
// names without a migration. Addressing is by this name in
// POST /api/units/:id/state and in the `controls` arrays sent to clients.
inline String controlName(const CasambiUnit& unit, size_t index) {
    if (index >= unit.controls.size()) return String("");
    const UnitControl& c = unit.controls[index];
    size_t sameType = 0, ordinal = 0;
    for (size_t i = 0; i < unit.controls.size(); i++) {
        if (unit.controls[i].typeName == c.typeName) {
            if (i < index) ordinal++;
            sameType++;
        }
    }
    if (sameType <= 1) return c.typeName;
    return c.typeName + String((unsigned)ordinal);
}

struct CasambiGroup {
    uint8_t groupId;
    String name;
    std::vector<uint8_t> unitIds;

    CasambiGroup() : groupId(0), name("") {}
};

struct CasambiScene {
    uint8_t sceneId;
    String name;

    CasambiScene() : sceneId(0), name("") {}
};

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================

struct NetworkConfig {
    String networkId;
    String networkUuid;
    String networkName;
    uint8_t protocolVersion;
    int revision;

    std::vector<CasambiKey> keys;
    std::vector<CasambiUnit> units;
    std::vector<CasambiGroup> groups;
    std::vector<CasambiScene> scenes;

    // Auto-connect settings
    bool autoConnectEnabled;
    String autoConnectAddress;

    // Advertised name of the gateway chosen at provisioning time. The BLE
    // address we connect to is often a random static address that does NOT
    // match any unit's hardware `address`, so the name cannot be resolved from
    // the unit list — we keep the scanned name here for display instead.
    String gatewayName;

    // NTP server used for time synchronisation (UTC). Configurable at runtime.
    String ntpServer;

    // Casambi network password, persisted so `refresh` can reuse it without
    // re-prompting. Stored alongside the rest of the config in plaintext
    // (same as the WiFi password and the BLE keyStore keys).
    String casambiPassword;

    // Debug settings (per category; persisted so debug on/off is non-destructive)
    bool bleDebugEnabled;
    bool casambiDebugEnabled;
    bool webDebugEnabled;
    bool parseDebugEnabled;
    bool heapDebugEnabled;
    bool cloudDebugEnabled;

    NetworkConfig() : networkId(""), networkUuid(""), networkName(""),
                      protocolVersion(0), revision(0),
                      autoConnectEnabled(true), autoConnectAddress(""),
                      gatewayName(""),
                      ntpServer(NTP_SERVER_DEFAULT), casambiPassword(""),
                      bleDebugEnabled(false), casambiDebugEnabled(true),
                      webDebugEnabled(true),
                      parseDebugEnabled(false), heapDebugEnabled(false),
                      cloudDebugEnabled(false) {}

    // Get the best key (highest role)
    CasambiKey* getBestKey() {
        if (keys.empty()) return nullptr;

        CasambiKey* best = &keys[0];
        for (size_t i = 1; i < keys.size(); i++) {
            if (keys[i].role > best->role) {
                best = &keys[i];
            }
        }
        return best;
    }

    // Get unit by device ID
    CasambiUnit* getUnitById(uint8_t deviceId) {
        for (auto& unit : units) {
            if (unit.deviceId == deviceId) {
                return &unit;
            }
        }
        return nullptr;
    }

    // Get group by group ID
    CasambiGroup* getGroupById(uint8_t groupId) {
        for (auto& group : groups) {
            if (group.groupId == groupId) {
                return &group;
            }
        }
        return nullptr;
    }

    // Get scene by scene ID
    CasambiScene* getSceneById(uint8_t sceneId) {
        for (auto& scene : scenes) {
            if (scene.sceneId == sceneId) {
                return &scene;
            }
        }
        return nullptr;
    }

    // Check if config is valid
    bool isValid() const {
        return !networkId.isEmpty() &&
               protocolVersion >= MIN_PROTOCOL_VERSION &&
               protocolVersion <= MAX_PROTOCOL_VERSION;
    }
};

// ============================================================================
// LOCAL SETTINGS PRESERVATION
// ============================================================================

// Copy the fields that are *local* to this device — i.e. not part of the
// Casambi cloud configuration — from `local` into `fresh`. Called after a cloud
// refresh rebuilds `fresh` from downloaded data, so a refresh never silently
// resets user-chosen local settings. Keeping the full list in one place means a
// newly added local field only has to be added here once.
//
// networkId / networkUuid / casambiPassword are restored separately at the call
// site because they are derived from the refresh flow itself.
inline void preserveLocalSettings(const NetworkConfig& local, NetworkConfig& fresh) {
    fresh.autoConnectEnabled  = local.autoConnectEnabled;
    fresh.autoConnectAddress  = local.autoConnectAddress;
    fresh.gatewayName         = local.gatewayName;   // locally scanned, not in cloud
    fresh.ntpServer           = local.ntpServer;     // runtime-configurable, local
    fresh.bleDebugEnabled     = local.bleDebugEnabled;
    fresh.casambiDebugEnabled = local.casambiDebugEnabled;
    fresh.webDebugEnabled     = local.webDebugEnabled;
    fresh.parseDebugEnabled   = local.parseDebugEnabled;
    fresh.heapDebugEnabled    = local.heapDebugEnabled;
    fresh.cloudDebugEnabled   = local.cloudDebugEnabled;
}

// ============================================================================
// WIFI CREDENTIALS
// ============================================================================

struct WiFiCredentials {
    String ssid;
    String password;

    WiFiCredentials() : ssid(""), password("") {}

    bool isValid() const {
        return !ssid.isEmpty() && !password.isEmpty();
    }
};

#endif // NETWORK_CONFIG_H
