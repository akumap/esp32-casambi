/**
 * Casambi BLE Client Implementation
 *
 * Includes connection health monitoring, data packet parsing,
 * and unit state tracking with generic capability-based aux interpretation.
 */

#include "casambi_client.h"
#include "packet.h"
#include "../log/event_log.h"
#include <NimBLEDevice.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>

// Global instance pointer for static callback
static CasambiClient* g_clientInstance = nullptr;

CasambiClient::CasambiClient(NetworkConfig* config)
    : _config(config), _bleClient(nullptr), _authChar(nullptr),
      _state(ConnectionState::None), _keyExchange(nullptr), _encryption(nullptr),
      _mtu(0), _unitId(0), _flags(0), _outPacketCount(2), _inPacketCount(1), _origin(1),
      _connectedAddress(""), _connectTime(0), _lastNotificationTime(0),
      _totalReceivedPackets(0), _lastDisconnectReason(DisconnectReason::None),
      _lastDisconnectSource("-"), _lastRssi(0),
      _unitStateCallback(nullptr), _connStateCallback(nullptr) {
    memset(_nonce, 0, NONCE_SIZE);
    _mutex    = xSemaphoreCreateMutex();
    _encMutex = xSemaphoreCreateMutex();
    g_clientInstance = this;
}

CasambiClient::~CasambiClient() {
    g_clientInstance = nullptr;
    disconnect();
    if (_keyExchange) delete _keyExchange;
    if (_encryption) delete _encryption;
    if (_mutex)    vSemaphoreDelete(_mutex);
    if (_encMutex) vSemaphoreDelete(_encMutex);
}

bool CasambiClient::connect(const String& address) {
    // Serialize against the control setters (web/serial): they hold _mutex
    // across _sendOperation (including the GATT write), so once we own the
    // mutex no sender can still be using _bleClient/_authChar when the
    // reconnect path below frees them via deleteClient().
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        Serial.println("BLE: connect: mutex timeout, try again");
        return false;
    }

    // RSSI quality gate ("gateway re-roll", see config.h): all units of the
    // network advertise the same virtual address, so each connect lands on a
    // random physical unit. A weak link (typically a distant luminaire) is
    // dropped and re-connected — with a strong advertiser nearby (e.g. an
    // always-powered actor) the re-roll almost always lands there.
    bool ok = false;
    for (int reroll = 0; ; reroll++) {
        ok = _connectLocked(address);
        if (!ok || BLE_MIN_CONNECT_RSSI == 0) break;

        // Let the controller settle before judging the link — the reading
        // right after the connect can be far off. Keep feeding the WDT.
        unsigned long settleStart = millis();
        while (millis() - settleStart < BLE_RSSI_SETTLE_MS) {
            delay(50);
            esp_task_wdt_reset();
        }

        if (!isBLEConnected()) {
            // Link died while settling — count it as a failed roll.
            _disconnectLocked(DisconnectReason::BLELinkLoss, "connect");
            if (reroll >= BLE_RSSI_REROLL_MAX) { ok = false; break; }
            continue;
        }

        int rssi = _bleClient->getRssi();
        if (rssi != 0) _lastRssi = rssi;

        // Accept a good link, or one we cannot measure (rssi == 0).
        if (rssi == 0 || rssi >= BLE_MIN_CONNECT_RSSI) break;

        if (reroll >= BLE_RSSI_REROLL_MAX) {
            // Budget exhausted: connectivity beats quality. Accept the LAST
            // roll — a previous, better unit cannot be re-targeted anyway.
            EventLog::log(LOG_WARN, "BLE link weak (rssi=%d < %d), accepted after %d re-rolls",
                          rssi, BLE_MIN_CONNECT_RSSI, reroll);
            break;
        }

        EventLog::log(LOG_INFO, "BLE gateway re-roll %d/%d: rssi=%d < %d",
                      reroll + 1, BLE_RSSI_REROLL_MAX, rssi, BLE_MIN_CONNECT_RSSI);
        _disconnectLocked(DisconnectReason::UserRequested, "reroll");
        delay(300);  // brief gap so the peer sees the disconnect before we re-initiate
    }

    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::_connectLocked(const String& address) {
    if (bleDebugEnabled) {
        Serial.printf("BLE: Connecting to %s\n", address.c_str());
    }

    if (_state != ConnectionState::None) {
        Serial.println("BLE: Already connected/connecting, disconnecting first...");
        _disconnectLocked(DisconnectReason::UserRequested, "connect");
        delay(500);
    }

    // Reassigning the String can reallocate its buffer; the async_tcp task
    // copies it via getConnectedAddress(), so the write happens under the same
    // mutex (writers are loop-task only, mirroring the NetworkConfig strings).
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    _connectedAddress = address;
    if (g_configMutex) xSemaphoreGive(g_configMutex);

    _outPacketCount = 2;
    _inPacketCount = 1;
    _origin = 1;
    _totalReceivedPackets = 0;

    if (_bleClient) {
        // NimBLE tracks clients in an internal list; deleteClient() removes it
        // there before freeing. A plain `delete` would leave a dangling pointer
        // in that list. createClient() may also hand back a pooled client, so
        // always release the old one explicitly first.
        NimBLEDevice::deleteClient(_bleClient);
        _bleClient = nullptr;
    }
    _bleClient = NimBLEDevice::createClient();

    // Bound the blocking connect attempt: NimBLE's default (30 s) would eat
    // most of the 45 s task-WDT budget on the loop task. 10 s keeps well clear
    // of it (the WDT margin is reserved for the keepalive GATT read, whose
    // 30 s ATT procedure timeout cannot be shortened — see config.h).
    _bleClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);

    // Casambi gateways are reconnected by their stored MAC (no live scan). NimBLE
    // requires the peer address type; mirror the old Bluedroid default (public).
    // If a gateway turns out to advertise a random address, see concept 6.3.
    if (!_bleClient->connect(NimBLEAddress(std::string(address.c_str()), BLE_ADDR_PUBLIC))) {
        Serial.println("BLE: Connection failed");
        _lastDisconnectReason = DisconnectReason::BLELinkLoss;
        return false;
    }

    _setState(ConnectionState::Connected);
    _connectTime = millis();
    _lastNotificationTime = millis();
    Serial.println("BLE: Connected");

    NimBLERemoteService* service = _bleClient->getService(NimBLEUUID(CASAMBI_SERVICE_UUID));
    if (!service) {
        Serial.println("BLE: Service not found");
        _disconnectLocked(DisconnectReason::InternalError, "connect");
        return false;
    }

    _authChar = service->getCharacteristic(NimBLEUUID(CASAMBI_AUTH_CHAR_UUID));
    if (!_authChar) {
        Serial.println("BLE: Auth characteristic not found");
        _disconnectLocked(DisconnectReason::InternalError, "connect");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.println("BLE: Initializing key exchange...");
    }

    if (_keyExchange) delete _keyExchange;
    _keyExchange = new ECDHKeyExchange();

    if (!_keyExchange->generateKeyPair()) {
        Serial.println("BLE: Failed to generate key pair");
        _disconnectLocked(DisconnectReason::KeyExchangeFailed, "connect");
        return false;
    }

    if (!_readDeviceInfo()) {
        Serial.println("BLE: Failed to read device info");
        _disconnectLocked(DisconnectReason::InternalError, "connect");
        return false;
    }

    if (!_performKeyExchange()) {
        Serial.println("BLE: Key exchange failed");
        _disconnectLocked(DisconnectReason::KeyExchangeFailed, "connect");
        return false;
    }

    CasambiKey* key = _config->getBestKey();
    if (key) {
        if (!_authenticate()) {
            Serial.println("BLE: Authentication failed");
            _disconnectLocked(DisconnectReason::AuthFailed, "connect");
            return false;
        }
    } else {
        Serial.println("BLE: No keys - assuming Classic network");
        _setState(ConnectionState::Authenticated);
    }

    // Initial link RSSI; refreshed by every health check (~10 s).
    _lastRssi = _bleClient->getRssi();

    Serial.printf("BLE: Ready! (RSSI %d dBm)\n", _lastRssi);
    return true;
}

void CasambiClient::disconnect() {
    _disconnectInternal(DisconnectReason::UserRequested, "user");
}

void CasambiClient::_disconnectInternal(DisconnectReason reason, const char* source) {
    // Wait for any in-flight sender (they hold _mutex across the GATT write)
    // before tearing the link state down. If the mutex stays busy — the holders
    // are bounded by NimBLE's own timeouts — skip this attempt instead of
    // racing the holder; the periodic health check will retry.
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.println("BLE: disconnect skipped (mutex busy), will retry");
        return;
    }
    _disconnectLocked(reason, source);
    xSemaphoreGive(_mutex);
}

void CasambiClient::_disconnectLocked(DisconnectReason reason, const char* source) {
    if (_bleClient && _bleClient->isConnected()) {
        _bleClient->disconnect();
    }
    _lastDisconnectReason = reason;
    _lastDisconnectSource = source ? source : "-";
    _authChar = nullptr;

    // Acquire _encMutex so that any in-flight BLE-task notification handler
    // finishes before we free the encryption object. Holders keep the mutex
    // only for a single encrypt/decrypt (milliseconds), so this succeeds in
    // practice; the timeout is a safety net against a wedged holder.
    if (xSemaphoreTake(_encMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (_encryption) {
            delete _encryption;
            _encryption = nullptr;
        }
        xSemaphoreGive(_encMutex);
    } else {
        // Timeout: leave _encryption untouched. Deleting it would be a
        // use-after-free against the holder, and nulling it without the mutex
        // (the old fallback) both raced the holder's read and leaked the
        // object. Keeping it is safe: _setState(None) below stops the data
        // notification paths, senders check isAuthenticated() first, and the
        // next _performKeyExchange (or the destructor) deletes it under
        // _encMutex — so the object is reclaimed instead of leaking.
        EventLog::log(LOG_WARN, "BLE: encMutex busy at disconnect, encryption cleanup deferred");
    }

    _setState(ConnectionState::None, reason);

    if (reason != DisconnectReason::UserRequested) {
        Serial.printf("BLE: Disconnected (reason: %d)\n", static_cast<int>(reason));
    } else {
        Serial.println("BLE: Disconnected");
    }
}

void CasambiClient::_setState(ConnectionState newState, DisconnectReason reason) {
    ConnectionState oldState = _state;
    _state = newState;

    if (oldState != newState && _connStateCallback) {
        _connStateCallback(newState, reason);
    }
}

bool CasambiClient::isBLEConnected() const {
    return _bleClient && _bleClient->isConnected();
}

bool CasambiClient::sendKeepalive() {
    if (!isAuthenticated() || !_authChar || !isBLEConnected()) return false;

    // Skip the ATT round-trip while notifications are flowing — the link is
    // demonstrably alive then. The GATT read only runs after a real silence
    // window; against a half-dead link it can block for NimBLE's full 30 s
    // ATT procedure timeout, which the 45 s task WDT budget accounts for.
    if (millis() - _lastNotificationTime < BLE_KEEPALIVE_IDLE_MS) return true;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    // Re-validate under the lock: between the unlocked pre-check above and
    // acquiring _mutex, a sender on another task may have detected link loss
    // and run _disconnectLocked(), nulling _authChar — dereferencing the stale
    // pointer here would crash. A disconnect already happened then, so just
    // report failure without triggering another one.
    if (!isAuthenticated() || !_authChar || !isBLEConnected()) {
        xSemaphoreGive(_mutex);
        return false;
    }

    std::string value = _authChar->readValue();
    xSemaphoreGive(_mutex);

    if (value.length() == 0) {
        Serial.println("BLE: Keepalive failed - no response");
        _disconnectInternal(DisconnectReason::BLELinkLoss, "keepalive");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.printf("BLE: Keepalive OK (%d bytes)\n", value.length());
    }
    _lastNotificationTime = millis();
    return true;
}

unsigned long CasambiClient::getConnectionUptime() const {
    if (_state == ConnectionState::Authenticated && _connectTime > 0) {
        return millis() - _connectTime;
    }
    return 0;
}

String CasambiClient::getConnectedAddress() const {
    // Copy under g_configMutex: the loop task reassigns _connectedAddress in
    // _connectLocked() (which can reallocate the String buffer) while the
    // async_tcp task reads it for /api/status and the WebSocket hello. The
    // isAuthenticated() checks callers do first are TOCTOU against a re-roll.
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    String out = _connectedAddress;
    if (g_configMutex) xSemaphoreGive(g_configMutex);
    return out;
}

bool CasambiClient::checkConnectionHealth() {
    if (_state == ConnectionState::Authenticated) {
        if (!isBLEConnected()) {
            Serial.println("BLE: Silent disconnect detected! Link lost.");
            _disconnectInternal(DisconnectReason::BLELinkLoss, "silent");
            return false;
        }

        // Refresh the cached link RSSI (cheap HCI query, loop task). Keep the
        // previous value on failure (getRssi() returns 0 then) so a disconnect
        // right after still logs the last real reading.
        int rssi = _bleClient->getRssi();
        if (rssi != 0) _lastRssi = rssi;

        unsigned long silentDuration = millis() - _lastNotificationTime;
        if (silentDuration > 300000UL) {
            if (bleDebugEnabled) {
                Serial.printf("BLE: No data received for %lu seconds\n", silentDuration / 1000);
            }
        }
    }

    if (_state == ConnectionState::Error) {
        return false;
    }

    return (_state == ConnectionState::Authenticated);
}

// ============================================================================
// CONTROL FUNCTIONS
// ============================================================================

bool CasambiClient::setSceneLevel(uint8_t sceneId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return false;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return false;
    }

    uint16_t target = encodeTarget(sceneId, TARGET_TYPE_SCENE);
    std::vector<uint8_t> payload;

    if (level == 0xFF) {
        payload.push_back(0xFF);
        payload.push_back(0x05);
    } else {
        payload.push_back(level);
    }

    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setUnitLevel(uint8_t unitId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { level };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setGroupLevel(uint8_t groupId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { level };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setUnitVertical(uint8_t unitId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { vertical };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setGroupVertical(uint8_t groupId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { vertical };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setUnitTemperature(uint8_t unitId, uint16_t kelvin) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    uint8_t temp = kelvin / 50;
    std::vector<uint8_t> payload = { temp };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetTemperature), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t hue;
    uint8_t sat;
    rgbToHS(r, g, b, hue, sat);
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = {
        static_cast<uint8_t>(hue & 0xFF),
        static_cast<uint8_t>((hue >> 8) & 0xFF),
        sat
    };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetColor), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setUnitSlider(uint8_t unitId, uint8_t value) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { value };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

bool CasambiClient::setGroupSlider(uint8_t groupId, uint8_t value) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return false; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { value };
    bool ok = _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
    return ok;
}

// ============================================================================
// CONNECTION FLOW
// ============================================================================

bool CasambiClient::_readDeviceInfo() {
    if (bleDebugEnabled) {
        Serial.println("BLE: Reading device info...");
    }

    std::string value = _authChar->readValue();
    // 7 header bytes (type, version, mtu, unitId x2, flags x2) + 16-byte nonce
    if (value.length() < 7 + NONCE_SIZE) {
        Serial.printf("BLE: Invalid device info length: %d\n", value.length());
        return false;
    }

    const uint8_t* data = (const uint8_t*)value.data();

    uint8_t type = data[0];
    uint8_t version = data[1];

    if (type != 0x01) {
        Serial.printf("BLE: Unexpected type: 0x%02x\n", type);
        return false;
    }

    if (version != _config->protocolVersion) {
        if (bleDebugEnabled) {
            Serial.printf("BLE: Protocol version mismatch: %d != %d (continuing anyway)\n",
                          version, _config->protocolVersion);
        }
    }

    _mtu = data[2];
    _unitId = data[3] | (data[4] << 8);
    _flags = data[5] | (data[6] << 8);
    memcpy(_nonce, data + 7, NONCE_SIZE);

    if (bleDebugEnabled) {
        Serial.printf("BLE: MTU=%d, UnitID=%d, Flags=0x%04x\n", _mtu, _unitId, _flags);
        hexDump("BLE: Device nonce", _nonce, NONCE_SIZE);
    }

    if (_authChar->canNotify()) {
        _authChar->subscribe(true, _notifyCallback);
        if (bleDebugEnabled) {
            Serial.println("BLE: Notifications enabled");
        }
    }

    return true;
}

bool CasambiClient::_performKeyExchange() {
    if (bleDebugEnabled) {
        Serial.println("BLE: Performing ECDH key exchange...");
    }

    if (!_keyExchange) {
        Serial.println("BLE: Key exchange not initialized!");
        return false;
    }

    unsigned long startTime = millis();
    while (_state == ConnectionState::Connected && millis() - startTime < 5000) {
        delay(10);
        esp_task_wdt_reset();
    }

    if (_state != ConnectionState::KeyExchanged) {
        Serial.println("BLE: Timeout waiting for device public key");
        return false;
    }

    // Derive transport key once and cache it; _authenticate reuses _transportKey
    // so the expensive ECDH computation is only performed once per connection.
    _transportKey = _keyExchange->deriveTransportKey();

    if (xSemaphoreTake(_encMutex, portMAX_DELAY) == pdTRUE) {
        if (_encryption) delete _encryption;
        _encryption = new CasambiEncryption(_transportKey.data());
        xSemaphoreGive(_encMutex);
    }
    if (bleDebugEnabled) {
        Serial.println("BLE: Transport key derived, encryption initialized");
    }

    std::vector<uint8_t> pubKeyX = _keyExchange->getPublicKeyX();
    std::vector<uint8_t> pubKeyY = _keyExchange->getPublicKeyY();

    uint8_t keyResponse[66];
    keyResponse[0] = 0x02;
    memcpy(keyResponse + 1, pubKeyX.data(), 32);
    memcpy(keyResponse + 33, pubKeyY.data(), 32);
    keyResponse[65] = 0x01;

    uint32_t notifyCountBefore = _totalReceivedPackets;
    _authChar->writeValue(keyResponse, 66);
    if (bleDebugEnabled) {
        Serial.println("BLE: Sent our public key");
    }

    // Wait for the device's acknowledgment notification (1-byte ACK for our public key)
    // before proceeding to auth. Without this, we may send the auth packet while the
    // device is still processing our public key and it will be ignored.
    unsigned long ackWaitStart = millis();
    while (_totalReceivedPackets == notifyCountBefore &&
           _state != ConnectionState::Error &&
           millis() - ackWaitStart < 2000) {
        delay(10);
        esp_task_wdt_reset();
    }

    if (bleDebugEnabled) {
        if (_totalReceivedPackets > notifyCountBefore) {
            Serial.println("BLE: Public key acknowledged by device");
        } else {
            Serial.println("BLE: No ack for public key (proceeding anyway)");
        }
    }

    if (_state == ConnectionState::Error) {
        Serial.println("BLE: Key exchange error");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.println("BLE: Key exchange complete");
    }
    return true;
}

bool CasambiClient::_authenticate() {
    if (bleDebugEnabled) {
        Serial.println("BLE: Authenticating...");
    }

    CasambiKey* key = _config->getBestKey();
    if (!key) {
        Serial.println("BLE: No key available");
        return false;
    }

    // Reuse the transport key cached by _performKeyExchange; avoids a second ECDH computation.
    if (_transportKey.size() < AES_KEY_SIZE) {
        Serial.println("BLE: Transport key not available");
        return false;
    }

    uint8_t hashInput[AES_KEY_SIZE + NONCE_SIZE + AES_KEY_SIZE];
    memcpy(hashInput, key->key, AES_KEY_SIZE);
    memcpy(hashInput + AES_KEY_SIZE, _nonce, NONCE_SIZE);
    memcpy(hashInput + AES_KEY_SIZE + NONCE_SIZE, _transportKey.data(), AES_KEY_SIZE);

    uint8_t authDigest[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, hashInput, sizeof(hashInput));
    mbedtls_sha256_finish(&sha_ctx, authDigest);
    mbedtls_sha256_free(&sha_ctx);

    std::vector<uint8_t> authPacket;
    authPacket.push_back(_inPacketCount & 0xFF);
    authPacket.push_back((_inPacketCount >> 8) & 0xFF);
    authPacket.push_back((_inPacketCount >> 16) & 0xFF);
    authPacket.push_back((_inPacketCount >> 24) & 0xFF);
    authPacket.push_back(0x04);
    authPacket.push_back(key->id);
    for (int i = 0; i < 32; i++) {
        authPacket.push_back(authDigest[i]);
    }

    if (bleDebugEnabled) {
        Serial.printf("BLE: Sending auth with counter=%u\n", _inPacketCount);
    }
    _sendEncryptedPacket(authPacket, _inPacketCount);
    _inPacketCount++;

    unsigned long startTime = millis();
    while (_state != ConnectionState::Authenticated &&
           _state != ConnectionState::Error &&
           millis() - startTime < 5000) {
        delay(10);
        esp_task_wdt_reset();
    }

    if (_state != ConnectionState::Authenticated) {
        Serial.println("BLE: Authentication timeout/failed");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.println("BLE: Authenticated!");
    }
    return true;
}

// ============================================================================
// PACKET HANDLING
// ============================================================================

bool CasambiClient::_sendOperation(uint8_t opcode, uint16_t target, const std::vector<uint8_t>& payload) {
    if (!isAuthenticated() || !_encryption) {
        Serial.println("BLE: Not authenticated");
        return false;
    }

    if (!isBLEConnected()) {
        Serial.println("BLE: Link lost, cannot send operation");
        // Callers (the control setters) hold _mutex, so use the locked variant
        // directly — _disconnectInternal would deadlock on the same mutex.
        _disconnectLocked(DisconnectReason::BLELinkLoss, "send");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.printf("BLE: Sending operation - opcode=0x%02x, target=0x%04x, payload_len=%d\n",
                      opcode, target, payload.size());
    }

    // Snapshot the counters so a failed send can be rolled back: _buildOperation
    // advances _origin, and _outPacketCount seeds the nonce. If the packet is
    // never handed to the GATT stack, both must stay put so the next send reuses
    // the same nonce/origin instead of silently skipping a value.
    uint16_t originBefore = _origin;

    std::vector<uint8_t> opPacket = _buildOperation(opcode, target, payload);

    std::vector<uint8_t> fullPacket;
    fullPacket.push_back(_outPacketCount & 0xFF);
    fullPacket.push_back((_outPacketCount >> 8) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 16) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 24) & 0xFF);
    fullPacket.push_back(0x07);
    fullPacket.insert(fullPacket.end(), opPacket.begin(), opPacket.end());

    if (!_sendEncryptedPacket(fullPacket, _outPacketCount)) {
        _origin = originBefore;   // roll back; nothing was transmitted
        return false;
    }
    _outPacketCount++;
    return true;
}

std::vector<uint8_t> CasambiClient::_buildOperation(uint8_t opcode, uint16_t target,
                                                     const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;

    uint16_t flags = (OPERATION_LIFETIME << 11) | payload.size();
    packet.push_back((flags >> 8) & 0xFF);
    packet.push_back(flags & 0xFF);
    packet.push_back(opcode);
    packet.push_back((_origin >> 8) & 0xFF);
    packet.push_back(_origin & 0xFF);
    _origin++;
    packet.push_back((target >> 8) & 0xFF);
    packet.push_back(target & 0xFF);
    packet.push_back(0x00);
    packet.push_back(0x00);
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

bool CasambiClient::_sendEncryptedPacket(const std::vector<uint8_t>& packet, uint32_t counter) {
    if (xSemaphoreTake(_encMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("BLE: _sendEncryptedPacket: encMutex timeout");
        return false;
    }

    if (!_encryption || !_authChar) {
        Serial.println("BLE: Encryption not initialized");
        xSemaphoreGive(_encMutex);
        return false;
    }

    std::vector<uint8_t> nonce = _getNonce(counter);
    std::vector<uint8_t> encrypted = _encryption->encryptThenMac(packet, nonce);

    // Release the mutex before writeValue: the BLE stack can fire the response
    // notification synchronously during writeValue (same FreeRTOS tick), so
    // _handleAuthNotification / _handleDataNotification must be able to acquire
    // _encMutex immediately — holding it across writeValue would cause a 200 ms
    // timeout and silently drop the notification.
    xSemaphoreGive(_encMutex);

    if (encrypted.empty()) {
        Serial.println("BLE: Encryption produced empty ciphertext");
        return false;
    }

    if (bleDebugEnabled) {
        Serial.printf("BLE: Sending encrypted packet - counter=%u, plaintext_len=%d, encrypted_len=%d\n",
                      counter, packet.size(), encrypted.size());
    }

    // Write without response (Casambi control writes are unacknowledged), so a
    // true return only means the stack accepted the buffer, not that the peer
    // received it. It still distinguishes a queued write from an outright
    // failure (disconnected characteristic, full tx buffer).
    if (!_authChar->writeValue(encrypted.data(), encrypted.size(), false)) {
        Serial.println("BLE: GATT writeValue failed");
        return false;
    }
    return true;
}

std::vector<uint8_t> CasambiClient::_getNonce(uint32_t counter) {
    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), _nonce, 4);
    nonce[4] = counter & 0xFF;
    nonce[5] = (counter >> 8) & 0xFF;
    nonce[6] = (counter >> 16) & 0xFF;
    nonce[7] = (counter >> 24) & 0xFF;
    memcpy(nonce.data() + 8, _nonce + 8, 8);
    return nonce;
}

// ============================================================================
// BLE NOTIFICATION HANDLING
// ============================================================================

void CasambiClient::_notifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (g_clientInstance) {
        g_clientInstance->_handleNotification(data, len);
    }
}

void CasambiClient::_handleNotification(uint8_t* data, size_t len) {
    if (len == 0) return;

    _lastNotificationTime = millis();
    _totalReceivedPackets++;

    if (bleDebugEnabled) {
        Serial.printf("BLE: Notification received (%d bytes, total #%u)\n", len, _totalReceivedPackets);
    }

    switch (_state) {
        case ConnectionState::Connected:
            _handleKeyExchangeNotification(data, len);
            break;

        case ConnectionState::KeyExchanged:
            if (len < 10) {
                if (bleDebugEnabled) {
                    Serial.printf("BLE: Key exchange acknowledgment received (%d bytes)\n", len);
                }
            } else {
                _handleAuthNotification(data, len);
            }
            break;

        case ConnectionState::Authenticated:
            _handleDataNotification(data, len);
            break;

        default:
            Serial.printf("BLE: Unexpected notification in state %d\n", static_cast<int>(_state.load()));
            break;
    }
}

void CasambiClient::_handleKeyExchangeNotification(uint8_t* data, size_t len) {
    if (len < 65) {
        Serial.printf("BLE: Invalid key exchange packet length: %d\n", len);
        _setState(ConnectionState::Error, DisconnectReason::KeyExchangeFailed);
        return;
    }

    if (data[0] != 0x02) {
        Serial.printf("BLE: Unexpected packet type during key exchange: 0x%02x\n", data[0]);
        _setState(ConnectionState::Error, DisconnectReason::KeyExchangeFailed);
        return;
    }

    const uint8_t* deviceKeyX = data + 1;
    const uint8_t* deviceKeyY = data + 33;

    if (bleDebugEnabled) {
        Serial.println("BLE: Received device public key");
    }

    if (!_keyExchange) {
        Serial.println("BLE: Key exchange not initialized!");
        _setState(ConnectionState::Error, DisconnectReason::InternalError);
        return;
    }

    if (!_keyExchange->setDevicePublicKey(deviceKeyX, deviceKeyY)) {
        Serial.println("BLE: Failed to set device public key");
        _setState(ConnectionState::Error, DisconnectReason::KeyExchangeFailed);
        return;
    }

    _setState(ConnectionState::KeyExchanged);
}

void CasambiClient::_handleAuthNotification(uint8_t* data, size_t len) {
    if (xSemaphoreTake(_encMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (!_encryption) {
        Serial.println("BLE: Encryption not initialized");
        xSemaphoreGive(_encMutex);
        _setState(ConnectionState::Error, DisconnectReason::InternalError);
        return;
    }

    if (len < CMAC_SIZE + 5) {
        Serial.printf("BLE: Auth response too short: %d\n", len);
        xSemaphoreGive(_encMutex);
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
        return;
    }

    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    if (bleDebugEnabled) {
        Serial.printf("BLE: Auth response packet (counter=%u, len=%d)\n", counter, len);
    }

    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), data, 4);
    memcpy(nonce.data() + 4, _nonce + 4, 12);

    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);
    xSemaphoreGive(_encMutex);

    if (plaintext.size() == 0) {
        Serial.println("BLE: Auth response decryption failed");
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
        return;
    }

    uint8_t responseType = plaintext[0];

    if (responseType == 0x05) {
        if (bleDebugEnabled) {
            Serial.println("BLE: Authentication successful!");
        }
        _setState(ConnectionState::Authenticated);
    } else if (responseType == 0x06) {
        Serial.println("BLE: Authentication rejected by device");
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
    } else {
        Serial.printf("BLE: Unexpected auth response type: 0x%02x\n", responseType);
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
    }
}

void CasambiClient::_handleDataNotification(uint8_t* data, size_t len) {
    if (len < CMAC_SIZE + 5) {
        if (bleDebugEnabled) {
            Serial.printf("BLE: Data packet too short: %d bytes\n", len);
        }
        return;
    }

    // Hold _encMutex only for the decrypt call so _disconnectInternal cannot
    // free _encryption while we are using it (use-after-free prevention).
    if (xSemaphoreTake(_encMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (!_encryption) {
        xSemaphoreGive(_encMutex);
        return;
    }

    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), data, 4);
    memcpy(nonce.data() + 4, _nonce + 4, 12);

    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);
    xSemaphoreGive(_encMutex);

    if (plaintext.size() == 0) {
        if (bleDebugEnabled) {
            Serial.println("BLE: Data packet decryption failed");
        }
        return;
    }

    uint8_t packetType = plaintext[0];

    if (bleDebugEnabled) {
        Serial.printf("BLE: Data packet type=0x%02x, decrypted_len=%d, counter=0x%08x\n",
                      packetType, plaintext.size(), counter);
    }

    const uint8_t* payload = plaintext.data() + 1;
    size_t payloadLen = plaintext.size() - 1;

    switch (packetType) {
        case 0x06: {
            // Unit state change event — one record per changed unit
            std::vector<UnitStateInfo> states;
            if (parseStatusBroadcast(payload, payloadLen, states)) {
                _applyUnitStates(states);
            } else {
                hexDump("BLE: Unparsed 0x06 payload", payload, payloadLen);
            }
            break;
        }

        case 0x07: {
            // Operation echo from other controllers
            OperationEcho echo;
            if (parseOperationEcho(payload, payloadLen, echo)) {
                if (casambiDebugEnabled) {
                    Serial.printf("Casambi: Echo %s %s[%d]",
                                  opcodeName(echo.opcode),
                                  targetTypeName(echo.targetType),
                                  echo.targetId);
                }
                if (echo.opcode == static_cast<uint8_t>(OpCode::SetLevel) && !echo.payload.empty()) {
                    if (casambiDebugEnabled) Serial.printf(" level=%d", echo.payload[0]);

                    if (echo.targetType == TARGET_TYPE_UNIT) {
                        UnitStateInfo info;
                        info.unitId = echo.targetId;
                        info.level = echo.payload[0];
                        info.on = (echo.payload[0] > 0);
                        info.online = true;
                        info.hasLevel = true;
                        std::vector<UnitStateInfo> states = { info };
                        _applyUnitStates(states);
                    }
                }
                if (casambiDebugEnabled) Serial.println();
            }
            break;
        }

        case 0x08: {
            if (bleDebugEnabled) {
                Serial.printf("BLE: <<< Unit state (0x08) %d bytes\n", payloadLen);
                hexDump("BLE: 0x08", payload, payloadLen);
            }
            std::vector<UnitStateInfo> states;
            if (parseUnitStateUpdate(payload, payloadLen, states)) {
                _applyUnitStates(states);
            }
            break;
        }

        case 0x09: {
            if (bleDebugEnabled) {
                Serial.printf("BLE: <<< Network state (0x09) %d bytes\n", payloadLen);
                hexDump("BLE: 0x09", payload, payloadLen);
            }
            if (parseDebugEnabled) {
                Serial.printf("P09 raw (%d):", payloadLen);
                for (size_t i = 0; i < payloadLen; i++) Serial.printf(" %02x", payload[i]);
                Serial.println();

                // P09 = scene/config revision tracker (confirmed). Structure:
                //   [0x02] [3-byte records...] [0x00 terminator]
                //
                // Record classes (position of 0x80 node-flag determines class):
                //   A  [0x80|id][gw][seq]       entity, config-gateway, revision seq
                //   B  [b0][0x80|id][b2]         relay node; b0/b2 semantics unknown
                //   C  chain record — b2 carries 0x80|next_entity (or 0x00 to end chain):
                //        header:     [reporter][ctx][0x80|first]  no prior chain context
                //        link:       [gw_prev][seq_prev][0x80|next]  encodes prev entity's gw/seq
                //        terminator: [gw_last][seq_last][0x00]   ends chain, b0 != 0x00
                //
                // P09 tracks VERSIONS only — it does NOT encode scene membership.
                // Scene membership must come from the cloud config (LittleFS).
                //
                // Entity lookup (Class A/C): scene before unit to resolve ID collisions
                //   (scene10 and unit10 share ID=10; scene wins in P09 context).
                // Gateway split: gw=unit2 → scene entities; gw=unit10 → unit/group entities.
                //
                // Seq counter semantics (confirmed):
                //   - Per-entity, persists across sessions
                //   - +2 per save screen: screen 1 (member list), screen 2 (properties)
                //   - Even seq = active/committed entry
                //   - Odd  seq = deleted/tombstone entry (scene was removed from network)
                //     All deleted scene IDs (11,12,14,17) have seq=5 in the known snapshot,
                //     suggesting they were at seq=4 when deleted (bumped to 5 as marker).
                //
                // Packet size: ≤8 B = single delta (one entity changed);
                //              >8 B = full snapshot sent on connect.
                if (payloadLen > 1 && payload[0] == 0x02) {
                    // scene first: resolves ID collisions (e.g. scene10 vs unit10)
                    auto printEntity = [this](uint8_t id) {
                        if (CasambiScene* s = _config->getSceneById(id)) { Serial.printf("scene%d(%s)", id, s->name.c_str()); return; }
                        if (CasambiGroup* g = _config->getGroupById(id)) { Serial.printf("grp%d(%s)",   id, g->name.c_str()); return; }
                        if (CasambiUnit*  u = _config->getUnitById(id))  { Serial.printf("unit%d(%s)",  id, u->name.c_str()); return; }
                        Serial.printf("?%d", id);
                    };
                    // unit first: gateways and relay nodes are physical devices
                    auto printDevice = [this](uint8_t id) {
                        if (CasambiUnit*  u = _config->getUnitById(id))  { Serial.printf("unit%d(%s)",  id, u->name.c_str()); return; }
                        if (CasambiGroup* g = _config->getGroupById(id)) { Serial.printf("grp%d(%s)",   id, g->name.c_str()); return; }
                        Serial.printf("?%d", id);
                    };

                    bool isDelta = (payloadLen <= 8);
                    if (!isDelta) Serial.printf("P09 snapshot:\n");

                    bool inChain = false;
                    uint8_t chainPrev = 0;
                    size_t i = 1;
                    int nA = 0, nB = 0, nC = 0, nUnk = 0;
                    while (i + 2 < payloadLen) {
                        uint8_t b0 = payload[i], b1 = payload[i+1], b2 = payload[i+2];
                        if (b0 == 0x00) break;
                        if (b0 & 0x80) {
                            // Class A: entity / gateway / seq
                            inChain = false;
                            Serial.printf(isDelta ? "P09 Δ " : "  A ");
                            printEntity(b0 & 0x7F);
                            Serial.printf(" seq=%d gw=", b2);
                            printDevice(b1);
                            Serial.println();
                            nA++;
                        } else if (b1 & 0x80) {
                            // Class B: relay node, raw bytes
                            inChain = false;
                            if (!isDelta) {
                                Serial.printf("  B relay=");
                                printDevice(b1 & 0x7F);
                                Serial.printf(" %02x ?? %02x\n", b0, b2);
                            }
                            nB++;
                        } else if (b2 & 0x80) {
                            // Class C: chain header or chain link
                            if (!isDelta) {
                                if (!inChain) {
                                    // header: b0=reporter, b1=ctx, b2&0x7F=first entity
                                    Serial.printf("  C[ ");
                                    printDevice(b0);
                                    Serial.printf(" ctx=%02x → ", b1);
                                    printEntity(b2 & 0x7F);
                                    Serial.println();
                                } else {
                                    // link: b0=gw, b1=seq for chainPrev, b2&0x7F=next entity
                                    Serial.printf("  C  ");
                                    printEntity(chainPrev);
                                    Serial.printf(" seq=%d gw=", b1);
                                    printDevice(b0);
                                    Serial.printf(" → ");
                                    printEntity(b2 & 0x7F);
                                    Serial.println();
                                }
                            }
                            chainPrev = b2 & 0x7F;
                            inChain = true;
                            nC++;
                        } else if (inChain) {
                            // Class C terminator: b0=gw, b1=seq for chainPrev, b2=0x00
                            if (!isDelta) {
                                Serial.printf("  C  ");
                                printEntity(chainPrev);
                                Serial.printf(" seq=%d gw=", b1);
                                printDevice(b0);
                                Serial.printf(" [end]\n");
                            }
                            inChain = false;
                            nC++;
                        } else {
                            // Unclassified
                            if (!isDelta) Serial.printf("  ? %02x %02x %02x\n", b0, b1, b2);
                            nUnk++;
                        }
                        i += 3;
                    }
                    if (!isDelta) Serial.printf("  (%d config, %d relay, %d chain, %d unknown)\n", nA, nB, nC, nUnk);
                } else if (payloadLen > 0) {
                    Serial.printf("P09 bad preamble: 0x%02x\n", payload[0]);
                }
            }
            break;
        }

        case 0x0A: {
            if (bleDebugEnabled) {
                Serial.println("BLE: <<< Time sync (0x0A)");
            }
            break;
        }

        case 0x0C: {
            if (bleDebugEnabled) {
                Serial.println("BLE: <<< Keepalive (0x0C)");
            }
            break;
        }

        default: {
            Serial.printf("BLE: <<< Unknown 0x%02x (%d bytes)\n", packetType, payloadLen);
            hexDump("BLE: Unknown", payload, payloadLen);
            break;
        }
    }
}

// ============================================================================
// STATE APPLICATION — generic capability-based aux interpretation
// ============================================================================

void CasambiClient::_applyUnitStates(const std::vector<UnitStateInfo>& states) {
    for (const auto& state : states) {
        CasambiUnit* unit = _config->getUnitById(state.unitId);

        if (!unit) {
            if (casambiDebugEnabled) {
                Serial.printf("Casambi: State for unknown unit %d (level=%d)\n",
                              state.unitId, state.level);
            }
            // Fire callback even for unknown units
            if (_unitStateCallback) {
                _unitStateCallback(state.unitId, state.level, state.online);
            }
            continue;
        }

        // Update basic state
        if (state.hasLevel) {
            unit->on = state.on;
            unit->online = state.online;
            unit->level = state.level;
        }

        // Interpret aux channels based on stored capabilities
        // Cap 0x23 (3 channels): aux1=vertical, aux2=colorTemp
        // Cap 0x13 (2 channels): aux1=vertical OR colorTemp (based on hasCCT/hasVertical)
        // Cap 0x03/0x00 (1 channel): no aux

        if (state.hasVertical && state.hasColorTemp) {
            // 2 aux channels: vertical + temp
            unit->vertical = state.vertical;
            unit->colorTemp = state.colorTemp;
        }
        else if (state.hasVertical) {
            // 1 aux channel: use unit capabilities to decide
            if (unit->hasVertical && unit->hasCCT) {
                // Shouldn't happen for 1-aux, but store as vertical
                unit->vertical = state.vertical;
            } else if (unit->hasVertical) {
                unit->vertical = state.vertical;
            } else if (unit->hasCCT) {
                unit->colorTemp = state.vertical;  // aux1 is actually CCT
            } else {
                // Unknown aux — store in vertical as fallback
                unit->vertical = state.vertical;
            }
        }

        // Log state change
        if (casambiDebugEnabled) {
            Serial.printf("Casambi: Unit [%d] '%s' -> level=%d %s",
                          unit->deviceId, unit->name.c_str(),
                          unit->level, unit->on ? "ON" : "OFF");
            if (unit->hasVertical) Serial.printf(" v=%d", unit->vertical);
            if (unit->hasCCT) Serial.printf(" t=%d", unit->colorTemp);
            if (!state.online) Serial.print(" OFFLINE");
            Serial.println();
        }

        // Fire callback
        if (_unitStateCallback) {
            _unitStateCallback(state.unitId, state.level, state.online);
        }
    }
}

