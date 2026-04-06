/**
 * Casambi BLE Client Implementation
 *
 * Includes connection health monitoring, data packet parsing,
 * and unit state tracking with generic capability-based aux interpretation.
 */

#include "casambi_client.h"
#include "packet.h"
#include <mbedtls/sha256.h>

// Global instance pointer for static callback
static CasambiClient* g_clientInstance = nullptr;

CasambiClient::CasambiClient(NetworkConfig* config)
    : _config(config), _bleClient(nullptr), _authChar(nullptr),
      _state(ConnectionState::None), _keyExchange(nullptr), _encryption(nullptr),
      _mtu(0), _unitId(0), _flags(0), _outPacketCount(2), _inPacketCount(1), _origin(1),
      _connectedAddress(""), _connectTime(0), _lastNotificationTime(0),
      _totalReceivedPackets(0), _lastDisconnectReason(DisconnectReason::None),
      _unitStateCallback(nullptr), _connStateCallback(nullptr) {
    memset(_nonce, 0, NONCE_SIZE);
    _mutex = xSemaphoreCreateMutex();
    g_clientInstance = this;
}

CasambiClient::~CasambiClient() {
    g_clientInstance = nullptr;
    disconnect();
    if (_keyExchange) delete _keyExchange;
    if (_encryption) delete _encryption;
    if (_mutex) vSemaphoreDelete(_mutex);
}

bool CasambiClient::connect(const String& address) {
    if (debugEnabled) {
        Serial.printf("BLE: Connecting to %s\n", address.c_str());
    }

    if (_state != ConnectionState::None) {
        Serial.println("BLE: Already connected/connecting, disconnecting first...");
        _disconnectInternal(DisconnectReason::UserRequested);
        delay(500);
    }

    _connectedAddress = address;

    _outPacketCount = 2;
    _inPacketCount = 1;
    _origin = 1;
    _totalReceivedPackets = 0;

    if (_bleClient) {
        delete _bleClient;
        _bleClient = nullptr;
    }
    _bleClient = BLEDevice::createClient();

    if (!_bleClient->connect(BLEAddress(address.c_str()))) {
        Serial.println("BLE: Connection failed");
        _lastDisconnectReason = DisconnectReason::BLELinkLoss;
        return false;
    }

    _setState(ConnectionState::Connected);
    _connectTime = millis();
    _lastNotificationTime = millis();
    Serial.println("BLE: Connected");

    BLERemoteService* service = _bleClient->getService(BLEUUID(CASAMBI_SERVICE_UUID));
    if (!service) {
        Serial.println("BLE: Service not found");
        _disconnectInternal(DisconnectReason::InternalError);
        return false;
    }

    _authChar = service->getCharacteristic(BLEUUID(CASAMBI_AUTH_CHAR_UUID));
    if (!_authChar) {
        Serial.println("BLE: Auth characteristic not found");
        _disconnectInternal(DisconnectReason::InternalError);
        return false;
    }

    if (debugEnabled) {
        Serial.println("BLE: Initializing key exchange...");
    }

    if (_keyExchange) delete _keyExchange;
    _keyExchange = new ECDHKeyExchange();

    if (!_keyExchange->generateKeyPair()) {
        Serial.println("BLE: Failed to generate key pair");
        _disconnectInternal(DisconnectReason::KeyExchangeFailed);
        return false;
    }

    if (!_readDeviceInfo()) {
        Serial.println("BLE: Failed to read device info");
        _disconnectInternal(DisconnectReason::InternalError);
        return false;
    }

    if (!_performKeyExchange()) {
        Serial.println("BLE: Key exchange failed");
        _disconnectInternal(DisconnectReason::KeyExchangeFailed);
        return false;
    }

    CasambiKey* key = _config->getBestKey();
    if (key) {
        if (!_authenticate()) {
            Serial.println("BLE: Authentication failed");
            _disconnectInternal(DisconnectReason::AuthFailed);
            return false;
        }
    } else {
        Serial.println("BLE: No keys - assuming Classic network");
        _setState(ConnectionState::Authenticated);
    }

    Serial.println("BLE: Ready!");
    return true;
}

void CasambiClient::disconnect() {
    _disconnectInternal(DisconnectReason::UserRequested);
}

void CasambiClient::_disconnectInternal(DisconnectReason reason) {
    if (_bleClient && _bleClient->isConnected()) {
        _bleClient->disconnect();
    }
    _lastDisconnectReason = reason;
    _authChar = nullptr;

    if (_encryption) {
        delete _encryption;
        _encryption = nullptr;
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

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    std::string value = _authChar->readValue();
    xSemaphoreGive(_mutex);

    if (value.length() == 0) {
        Serial.println("BLE: Keepalive failed - no response");
        _disconnectInternal(DisconnectReason::BLELinkLoss);
        return false;
    }

    if (debugEnabled) {
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

bool CasambiClient::checkConnectionHealth() {
    if (_state == ConnectionState::Authenticated) {
        if (!isBLEConnected()) {
            Serial.println("BLE: Silent disconnect detected! Link lost.");
            _disconnectInternal(DisconnectReason::BLELinkLoss);
            return false;
        }

        unsigned long silentDuration = millis() - _lastNotificationTime;
        if (silentDuration > 300000UL) {
            if (debugEnabled) {
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

void CasambiClient::setSceneLevel(uint8_t sceneId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(sceneId, TARGET_TYPE_SCENE);
    std::vector<uint8_t> payload;

    if (level == 0xFF) {
        payload.push_back(0xFF);
        payload.push_back(0x05);
    } else {
        payload.push_back(level);
    }

    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitLevel(uint8_t unitId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { level };
    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupLevel(uint8_t groupId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { level };
    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitVertical(uint8_t unitId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { vertical };
    _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupVertical(uint8_t groupId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { vertical };
    _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitTemperature(uint8_t unitId, uint16_t kelvin) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    uint8_t temp = kelvin / 50;
    std::vector<uint8_t> payload = { temp };
    _sendOperation(static_cast<uint8_t>(OpCode::SetTemperature), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t hue;
    uint8_t sat;
    rgbToHS(r, g, b, hue, sat);
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = {
        static_cast<uint8_t>(hue & 0xFF),
        static_cast<uint8_t>((hue >> 8) & 0xFF),
        sat
    };
    _sendOperation(static_cast<uint8_t>(OpCode::SetColor), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitSlider(uint8_t unitId, uint8_t value) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { value };
    _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupSlider(uint8_t groupId, uint8_t value) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!isAuthenticated()) { xSemaphoreGive(_mutex); return; }
    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { value };
    _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
}

// ============================================================================
// CONNECTION FLOW
// ============================================================================

bool CasambiClient::_readDeviceInfo() {
    if (debugEnabled) {
        Serial.println("BLE: Reading device info...");
    }

    std::string value = _authChar->readValue();
    if (value.length() < 21) {
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
        if (debugEnabled) {
            Serial.printf("BLE: Protocol version mismatch: %d != %d (continuing anyway)\n",
                          version, _config->protocolVersion);
        }
    }

    _mtu = data[2];
    _unitId = data[3] | (data[4] << 8);
    _flags = data[5] | (data[6] << 8);
    memcpy(_nonce, data + 7, NONCE_SIZE);

    if (debugEnabled) {
        Serial.printf("BLE: MTU=%d, UnitID=%d, Flags=0x%04x\n", _mtu, _unitId, _flags);
        hexDump("BLE: Device nonce", _nonce, NONCE_SIZE);
    }

    if (_authChar->canNotify()) {
        _authChar->registerForNotify(_notifyCallback);
        if (debugEnabled) {
            Serial.println("BLE: Notifications enabled");
        }
    }

    return true;
}

bool CasambiClient::_performKeyExchange() {
    if (debugEnabled) {
        Serial.println("BLE: Performing ECDH key exchange...");
    }

    if (!_keyExchange) {
        Serial.println("BLE: Key exchange not initialized!");
        return false;
    }

    unsigned long startTime = millis();
    while (_state == ConnectionState::Connected && millis() - startTime < 5000) {
        delay(10);
    }

    if (_state != ConnectionState::KeyExchanged) {
        Serial.println("BLE: Timeout waiting for device public key");
        return false;
    }

    std::vector<uint8_t> transportKey = _keyExchange->deriveTransportKey();

    if (_encryption) delete _encryption;
    _encryption = new CasambiEncryption(transportKey.data());
    if (debugEnabled) {
        Serial.println("BLE: Transport key derived, encryption initialized");
    }

    std::vector<uint8_t> pubKeyX = _keyExchange->getPublicKeyX();
    std::vector<uint8_t> pubKeyY = _keyExchange->getPublicKeyY();

    uint8_t keyResponse[66];
    keyResponse[0] = 0x02;
    memcpy(keyResponse + 1, pubKeyX.data(), 32);
    memcpy(keyResponse + 33, pubKeyY.data(), 32);
    keyResponse[65] = 0x01;

    _authChar->writeValue(keyResponse, 66);
    if (debugEnabled) {
        Serial.println("BLE: Sent our public key");
    }

    delay(100);

    if (_state == ConnectionState::Error) {
        Serial.println("BLE: Key exchange error");
        return false;
    }

    if (debugEnabled) {
        Serial.println("BLE: Key exchange complete");
    }
    return true;
}

bool CasambiClient::_authenticate() {
    if (debugEnabled) {
        Serial.println("BLE: Authenticating...");
    }

    CasambiKey* key = _config->getBestKey();
    if (!key) {
        Serial.println("BLE: No key available");
        return false;
    }

    std::vector<uint8_t> transportKey = _keyExchange->deriveTransportKey();

    uint8_t hashInput[AES_KEY_SIZE + NONCE_SIZE + AES_KEY_SIZE];
    memcpy(hashInput, key->key, AES_KEY_SIZE);
    memcpy(hashInput + AES_KEY_SIZE, _nonce, NONCE_SIZE);
    memcpy(hashInput + AES_KEY_SIZE + NONCE_SIZE, transportKey.data(), AES_KEY_SIZE);

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

    if (debugEnabled) {
        Serial.printf("BLE: Sending auth with counter=%u\n", _inPacketCount);
    }
    _sendEncryptedPacket(authPacket, _inPacketCount);
    _inPacketCount++;

    unsigned long startTime = millis();
    while (_state != ConnectionState::Authenticated &&
           _state != ConnectionState::Error &&
           millis() - startTime < 5000) {
        delay(10);
    }

    if (_state != ConnectionState::Authenticated) {
        Serial.println("BLE: Authentication timeout/failed");
        return false;
    }

    if (debugEnabled) {
        Serial.println("BLE: Authenticated!");
    }
    return true;
}

// ============================================================================
// PACKET HANDLING
// ============================================================================

void CasambiClient::_sendOperation(uint8_t opcode, uint16_t target, const std::vector<uint8_t>& payload) {
    if (!isAuthenticated() || !_encryption) {
        Serial.println("BLE: Not authenticated");
        return;
    }

    if (!isBLEConnected()) {
        Serial.println("BLE: Link lost, cannot send operation");
        _disconnectInternal(DisconnectReason::BLELinkLoss);
        return;
    }

    if (debugEnabled) {
        Serial.printf("BLE: Sending operation - opcode=0x%02x, target=0x%04x, payload_len=%d\n",
                      opcode, target, payload.size());
    }

    std::vector<uint8_t> opPacket = _buildOperation(opcode, target, payload);

    std::vector<uint8_t> fullPacket;
    fullPacket.push_back(_outPacketCount & 0xFF);
    fullPacket.push_back((_outPacketCount >> 8) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 16) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 24) & 0xFF);
    fullPacket.push_back(0x07);
    fullPacket.insert(fullPacket.end(), opPacket.begin(), opPacket.end());

    _sendEncryptedPacket(fullPacket, _outPacketCount);
    _outPacketCount++;
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

void CasambiClient::_sendEncryptedPacket(const std::vector<uint8_t>& packet, uint32_t counter) {
    if (!_encryption || !_authChar) {
        Serial.println("BLE: Encryption not initialized");
        return;
    }

    std::vector<uint8_t> nonce = _getNonce(counter);
    std::vector<uint8_t> encrypted = _encryption->encryptThenMac(packet, nonce);

    if (debugEnabled) {
        Serial.printf("BLE: Sending encrypted packet - counter=%u, plaintext_len=%d, encrypted_len=%d\n",
                      counter, packet.size(), encrypted.size());
    }

    _authChar->writeValue(encrypted.data(), encrypted.size());
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

void CasambiClient::_notifyCallback(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (g_clientInstance) {
        g_clientInstance->_handleNotification(data, len);
    }
}

void CasambiClient::_handleNotification(uint8_t* data, size_t len) {
    if (len == 0) return;

    _lastNotificationTime = millis();
    _totalReceivedPackets++;

    if (debugEnabled) {
        Serial.printf("BLE: Notification received (%d bytes, total #%u)\n", len, _totalReceivedPackets);
    }

    switch (_state) {
        case ConnectionState::Connected:
            _handleKeyExchangeNotification(data, len);
            break;

        case ConnectionState::KeyExchanged:
            if (len < 10) {
                if (debugEnabled) {
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
            Serial.printf("BLE: Unexpected notification in state %d\n", static_cast<int>(_state));
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

    if (debugEnabled) {
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
    if (!_encryption) {
        Serial.println("BLE: Encryption not initialized");
        _setState(ConnectionState::Error, DisconnectReason::InternalError);
        return;
    }

    if (len < CMAC_SIZE + 5) {
        Serial.printf("BLE: Auth response too short: %d\n", len);
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
        return;
    }

    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    if (debugEnabled) {
        Serial.printf("BLE: Auth response packet (counter=%u, len=%d)\n", counter, len);
    }

    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), data, 4);
    memcpy(nonce.data() + 4, _nonce + 4, 12);

    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);

    if (plaintext.size() == 0) {
        Serial.println("BLE: Auth response decryption failed");
        _setState(ConnectionState::Error, DisconnectReason::AuthFailed);
        return;
    }

    uint8_t responseType = plaintext[0];

    if (responseType == 0x05) {
        if (debugEnabled) {
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
    if (!_encryption) {
        Serial.println("BLE: Encryption not initialized");
        return;
    }

    if (len < CMAC_SIZE + 5) {
        if (debugEnabled) {
            Serial.printf("BLE: Data packet too short: %d bytes\n", len);
        }
        return;
    }

    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), data, 4);
    memcpy(nonce.data() + 4, _nonce + 4, 12);

    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);

    if (plaintext.size() == 0) {
        if (debugEnabled) {
            Serial.println("BLE: Data packet decryption failed");
        }
        return;
    }

    uint8_t packetType = plaintext[0];

    if (debugEnabled) {
        Serial.printf("BLE: Data packet type=0x%02x, decrypted_len=%d, counter=0x%08x\n",
                      packetType, plaintext.size(), counter);
    }

    const uint8_t* payload = plaintext.data() + 1;
    size_t payloadLen = plaintext.size() - 1;

    switch (packetType) {
        case 0x06: {
            // Status broadcast — unit state information
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
                Serial.printf("BLE: <<< Echo: %s %s[%d]",
                              opcodeName(echo.opcode),
                              targetTypeName(echo.targetType),
                              echo.targetId);
                if (echo.opcode == static_cast<uint8_t>(OpCode::SetLevel) && !echo.payload.empty()) {
                    Serial.printf(" level=%d", echo.payload[0]);

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
                Serial.println();
            }
            break;
        }

        case 0x08: {
            if (debugEnabled) {
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
            if (debugEnabled) {
                Serial.printf("BLE: <<< Network state (0x09) %d bytes\n", payloadLen);
                hexDump("BLE: 0x09", payload, payloadLen);
            }
            if (parseDebugEnabled) {
                Serial.printf("P09 (%d):", payloadLen);
                for (size_t i = 0; i < payloadLen; i++) Serial.printf(" %02x", payload[i]);
                Serial.println();
            }
            break;
        }

        case 0x0A: {
            if (debugEnabled) {
                Serial.println("BLE: <<< Time sync (0x0A)");
            }
            break;
        }

        case 0x0C: {
            if (debugEnabled) {
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
            if (debugEnabled) {
                Serial.printf("BLE: State for unknown unit %d (level=%d)\n",
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
        Serial.printf("BLE: Unit [%d] '%s' -> level=%d %s",
                      unit->deviceId, unit->name.c_str(),
                      unit->level, unit->on ? "ON" : "OFF");
        if (unit->hasVertical) Serial.printf(" v=%d", unit->vertical);
        if (unit->hasCCT) Serial.printf(" t=%d", unit->colorTemp);
        if (!state.online) Serial.print(" OFFLINE");
        Serial.println();

        // Fire callback
        if (_unitStateCallback) {
            _unitStateCallback(state.unitId, state.level, state.online);
        }
    }
}

