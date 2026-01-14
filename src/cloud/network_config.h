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

struct CasambiUnit {
    uint8_t deviceId;
    uint16_t type;
    String uuid;
    String address;
    String name;
    String firmware;
    bool online;
    bool on;

    CasambiUnit() : deviceId(0), type(0), uuid(""), address(""),
                    name(""), firmware(""), online(false), on(false) {}
};

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

    // Debug settings
    bool debugEnabled;

    NetworkConfig() : networkId(""), networkUuid(""), networkName(""),
                      protocolVersion(0), revision(0),
                      autoConnectEnabled(true), autoConnectAddress(""),
                      debugEnabled(false) {}

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
