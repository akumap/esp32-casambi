/**
 * Casambi BLE Client Implementation
 */

#include "casambi_client.h"
#include "packet.h"
#include <mbedtls/sha256.h>

// Global instance pointer for static callback
static CasambiClient* g_clientInstance = nullptr;

CasambiClient::CasambiClient(NetworkConfig* config)
    : _config(config), _bleClient(nullptr), _authChar(nullptr),
      _state(ConnectionState::None), _keyExchange(nullptr), _encryption(nullptr),
      _mtu(0), _unitId(0), _flags(0), _outPacketCount(2), _inPacketCount(1), _origin(1) {
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
        Serial.println("BLE: Already connected/connecting");
        disconnect();
        delay(500);
    }

    // Create BLE client
    if (!_bleClient) {
        _bleClient = BLEDevice::createClient();
    }

    // Connect to device
    if (!_bleClient->connect(BLEAddress(address.c_str()))) {
        Serial.println("BLE: Connection failed");
        return false;
    }

    _state = ConnectionState::Connected;
    Serial.println("BLE: Connected");

    // Get service
    BLERemoteService* service = _bleClient->getService(BLEUUID(CASAMBI_SERVICE_UUID));
    if (!service) {
        Serial.println("BLE: Service not found");
        disconnect();
        return false;
    }

    // Get auth characteristic
    _authChar = service->getCharacteristic(BLEUUID(CASAMBI_AUTH_CHAR_UUID));
    if (!_authChar) {
        Serial.println("BLE: Auth characteristic not found");
        disconnect();
        return false;
    }

    // Initialize ECDH BEFORE reading device info (which enables notifications)
    if (debugEnabled) {
        Serial.println("BLE: Initializing key exchange...");
    }
    if (!_keyExchange) {
        _keyExchange = new ECDHKeyExchange();
    }
    if (!_keyExchange->generateKeyPair()) {
        Serial.println("BLE: Failed to generate key pair");
        disconnect();
        return false;
    }

    // Read device info and perform handshake
    if (!_readDeviceInfo()) {
        Serial.println("BLE: Failed to read device info");
        disconnect();
        return false;
    }

    if (!_performKeyExchange()) {
        Serial.println("BLE: Key exchange failed");
        disconnect();
        return false;
    }

    // Authenticate if we have keys
    CasambiKey* key = _config->getBestKey();
    if (key) {
        if (!_authenticate()) {
            Serial.println("BLE: Authentication failed");
            disconnect();
            return false;
        }
    } else {
        Serial.println("BLE: No keys - assuming Classic network");
        _state = ConnectionState::Authenticated;
    }

    Serial.println("BLE: Ready!");
    return true;
}

void CasambiClient::disconnect() {
    if (_bleClient && _bleClient->isConnected()) {
        _bleClient->disconnect();
    }
    _state = ConnectionState::None;
    Serial.println("BLE: Disconnected");
}

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
        // Turn on to last level
        payload.push_back(0xFF);
        payload.push_back(0x05);
    } else {
        payload.push_back(level);
    }

    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitLevel(uint8_t unitId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { level };

    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupLevel(uint8_t groupId, uint8_t level) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { level };

    _sendOperation(static_cast<uint8_t>(OpCode::SetLevel), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitVertical(uint8_t unitId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { vertical };

    _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupVertical(uint8_t groupId, uint8_t vertical) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { vertical };

    _sendOperation(static_cast<uint8_t>(OpCode::SetVertical), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitTemperature(uint8_t unitId, uint16_t kelvin) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    uint8_t temp = kelvin / 50;
    std::vector<uint8_t> payload = { temp };

    _sendOperation(static_cast<uint8_t>(OpCode::SetTemperature), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

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
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(unitId, TARGET_TYPE_UNIT);
    std::vector<uint8_t> payload = { value };

    _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
}

void CasambiClient::setGroupSlider(uint8_t groupId, uint8_t value) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("Failed to acquire mutex (timeout)");
        return;
    }

    if (!isAuthenticated()) {
        Serial.println("Not authenticated");
        xSemaphoreGive(_mutex);
        return;
    }

    uint16_t target = encodeTarget(groupId, TARGET_TYPE_GROUP);
    std::vector<uint8_t> payload = { value };

    _sendOperation(static_cast<uint8_t>(OpCode::SetSlider), target, payload);
    xSemaphoreGive(_mutex);
}

bool CasambiClient::_readDeviceInfo() {
    if (debugEnabled) {
        Serial.println("BLE: Reading device info...");
    }

    // Read initial packet
    std::string value = _authChar->readValue();
    if (value.length() < 21) {
        Serial.printf("BLE: Invalid device info length: %d\n", value.length());
        return false;
    }

    const uint8_t* data = (const uint8_t*)value.data();

    // Parse packet: [type][version][mtu][unit][flags][nonce...]
    uint8_t type = data[0];
    uint8_t version = data[1];

    if (type != 0x01) {
        Serial.printf("BLE: Unexpected type: 0x%02x\n", type);
        return false;
    }

    if (version != _config->protocolVersion) {
        if (debugEnabled) {
            Serial.printf("BLE: Protocol version mismatch: %d != %d (continuing anyway)\n", version, _config->protocolVersion);
        }
    }

    _mtu = data[2];
    _unitId = data[3] | (data[4] << 8);
    _flags = data[5] | (data[6] << 8);

    // Copy nonce
    memcpy(_nonce, data + 7, NONCE_SIZE);

    if (debugEnabled) {
        Serial.printf("BLE: MTU=%d, UnitID=%d, Flags=0x%04x\n", _mtu, _unitId, _flags);
        Serial.print("BLE: Device nonce: ");
        for (int i = 0; i < NONCE_SIZE; i++) Serial.printf("%02x ", _nonce[i]);
        Serial.println();
    }

    // Start notifications
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

    // Key pair should already be generated before notifications were enabled
    if (!_keyExchange) {
        Serial.println("BLE: Key exchange not initialized!");
        return false;
    }

    // Wait for device public key notification (should arrive quickly)
    // The device sends its public key automatically after enabling notifications
    unsigned long startTime = millis();
    while (_state == ConnectionState::Connected && millis() - startTime < 5000) {
        delay(10);
    }

    if (_state != ConnectionState::KeyExchanged) {
        Serial.println("BLE: Timeout waiting for device public key");
        return false;
    }

    // Derive transport key immediately (we have both public keys now)
    std::vector<uint8_t> transportKey = _keyExchange->deriveTransportKey();

    // Create encryption object BEFORE sending our key (so we're ready for confirmation)
    if (_encryption) delete _encryption;
    _encryption = new CasambiEncryption(transportKey.data());
    if (debugEnabled) {
        Serial.println("BLE: Transport key derived, encryption initialized");
    }

    // Send our public key
    std::vector<uint8_t> pubKeyX = _keyExchange->getPublicKeyX();
    std::vector<uint8_t> pubKeyY = _keyExchange->getPublicKeyY();

    uint8_t keyResponse[66];
    keyResponse[0] = 0x02;  // Type: public key
    memcpy(keyResponse + 1, pubKeyX.data(), 32);
    memcpy(keyResponse + 33, pubKeyY.data(), 32);
    keyResponse[65] = 0x01;  // Confirmation

    _authChar->writeValue(keyResponse, 66);
    if (debugEnabled) {
        Serial.println("BLE: Sent our public key");
    }

    // Give device a moment to process (optional acknowledgment may arrive)
    delay(100);

    // Check if error occurred
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

    // Compute auth digest: SHA256(key || nonce || transport_key)
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

    // Build auth packet
    std::vector<uint8_t> authPacket;

    // Counter (little-endian)
    authPacket.push_back(_inPacketCount & 0xFF);
    authPacket.push_back((_inPacketCount >> 8) & 0xFF);
    authPacket.push_back((_inPacketCount >> 16) & 0xFF);
    authPacket.push_back((_inPacketCount >> 24) & 0xFF);

    // Type
    authPacket.push_back(0x04);

    // Key ID
    authPacket.push_back(key->id);

    // Auth digest
    for (int i = 0; i < 32; i++) {
        authPacket.push_back(authDigest[i]);
    }

    // Encrypt and send
    if (debugEnabled) {
        Serial.printf("BLE: Sending auth with counter=%u\n", _inPacketCount);
    }
    _sendEncryptedPacket(authPacket, _inPacketCount);
    _inPacketCount++;

    // Wait for auth response
    unsigned long startTime = millis();
    while (_state != ConnectionState::Authenticated && _state != ConnectionState::Error && millis() - startTime < 5000) {
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

void CasambiClient::_sendOperation(uint8_t opcode, uint16_t target, const std::vector<uint8_t>& payload) {
    if (!isAuthenticated() || !_encryption) {
        Serial.println("BLE: Not authenticated");
        return;
    }

    if (debugEnabled) {
        Serial.printf("BLE: Sending operation - opcode=0x%02x, target=0x%04x, payload_len=%d\n",
                      opcode, target, payload.size());
    }

    // Build operation packet
    std::vector<uint8_t> opPacket = _buildOperation(opcode, target, payload);

    // Build full packet with counter
    std::vector<uint8_t> fullPacket;

    // Counter (little-endian)
    fullPacket.push_back(_outPacketCount & 0xFF);
    fullPacket.push_back((_outPacketCount >> 8) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 16) & 0xFF);
    fullPacket.push_back((_outPacketCount >> 24) & 0xFF);

    // Type
    fullPacket.push_back(0x07);  // Operation

    // Operation data
    fullPacket.insert(fullPacket.end(), opPacket.begin(), opPacket.end());

    // Encrypt and send
    _sendEncryptedPacket(fullPacket, _outPacketCount);
    _outPacketCount++;
}

std::vector<uint8_t> CasambiClient::_buildOperation(uint8_t opcode, uint16_t target,
                                                     const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;

    // Flags: (lifetime << 11) | payload_length (BIG-ENDIAN)
    uint16_t flags = (OPERATION_LIFETIME << 11) | payload.size();
    packet.push_back((flags >> 8) & 0xFF);  // High byte first (big-endian)
    packet.push_back(flags & 0xFF);         // Low byte second

    // OpCode
    packet.push_back(opcode);

    // Origin (BIG-ENDIAN)
    packet.push_back((_origin >> 8) & 0xFF);  // High byte first (big-endian)
    packet.push_back(_origin & 0xFF);         // Low byte second
    _origin++;

    // Target (BIG-ENDIAN)
    packet.push_back((target >> 8) & 0xFF);  // High byte first (big-endian)
    packet.push_back(target & 0xFF);         // Low byte second

    // Reserved (BIG-ENDIAN)
    packet.push_back(0x00);
    packet.push_back(0x00);

    // Payload
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

void CasambiClient::_sendEncryptedPacket(const std::vector<uint8_t>& packet, uint32_t counter) {
    if (!_encryption || !_authChar) {
        Serial.println("BLE: Encryption not initialized");
        return;
    }

    // Get nonce for this counter
    std::vector<uint8_t> nonce = _getNonce(counter);

    // Encrypt with MAC
    std::vector<uint8_t> encrypted = _encryption->encryptThenMac(packet, nonce);

    if (debugEnabled) {
        Serial.printf("BLE: Sending encrypted packet - counter=%u, plaintext_len=%d, encrypted_len=%d\n",
                      counter, packet.size(), encrypted.size());
        Serial.print("BLE: Plaintext: ");
        for (size_t i = 0; i < packet.size() && i < 16; i++) {
            Serial.printf("%02x ", packet[i]);
        }
        Serial.println();
    }

    // Send via BLE
    _authChar->writeValue(encrypted.data(), encrypted.size());

    if (debugEnabled) {
        Serial.println("BLE: Packet sent");
    }
}

std::vector<uint8_t> CasambiClient::_getNonce(uint32_t counter) {
    std::vector<uint8_t> nonce(NONCE_SIZE);

    // First 4 bytes from device nonce
    memcpy(nonce.data(), _nonce, 4);

    // Next 4 bytes: counter (little-endian)
    nonce[4] = counter & 0xFF;
    nonce[5] = (counter >> 8) & 0xFF;
    nonce[6] = (counter >> 16) & 0xFF;
    nonce[7] = (counter >> 24) & 0xFF;

    // Last 8 bytes from device nonce
    memcpy(nonce.data() + 8, _nonce + 8, 8);

    return nonce;
}

void CasambiClient::_notifyCallback(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (g_clientInstance) {
        g_clientInstance->_handleNotification(data, len);
    }
}

void CasambiClient::_handleNotification(uint8_t* data, size_t len) {
    if (len == 0) return;

    if (debugEnabled) {
        Serial.printf("BLE: Notification received (%d bytes)\n", len);
    }

    // Route based on connection state
    switch (_state) {
        case ConnectionState::Connected:
            // Expecting device public key during ECDH exchange
            _handleKeyExchangeNotification(data, len);
            break;

        case ConnectionState::KeyExchanged:
            // Could be ECDH confirmation (small packet) or auth response (encrypted)
            if (len < 10) {
                // Small packet - likely just acknowledgment of key exchange
                if (debugEnabled) {
                    Serial.printf("BLE: Key exchange acknowledgment received (%d bytes)\n", len);
                }
                // Don't change state, auth will handle that
            } else {
                // Large packet - likely auth response
                _handleAuthNotification(data, len);
            }
            break;

        case ConnectionState::Authenticated:
            // Normal encrypted data packets
            _handleDataNotification(data, len);
            break;

        default:
            Serial.printf("BLE: Unexpected notification in state %d\n", static_cast<int>(_state));
            break;
    }
}

void CasambiClient::_handleKeyExchangeNotification(uint8_t* data, size_t len) {
    // Device public key packet: [type:1][x:32][y:32] = 65 bytes
    if (len < 65) {
        Serial.printf("BLE: Invalid key exchange packet length: %d\n", len);
        _state = ConnectionState::Error;
        return;
    }

    if (data[0] != 0x02) {
        Serial.printf("BLE: Unexpected packet type during key exchange: 0x%02x\n", data[0]);
        _state = ConnectionState::Error;
        return;
    }

    // Extract device public key
    const uint8_t* deviceKeyX = data + 1;
    const uint8_t* deviceKeyY = data + 33;

    if (debugEnabled) {
        Serial.println("BLE: Received device public key");
    }

    // Set device public key in ECDH context
    if (!_keyExchange) {
        Serial.println("BLE: Key exchange not initialized!");
        _state = ConnectionState::Error;
        return;
    }

    if (!_keyExchange->setDevicePublicKey(deviceKeyX, deviceKeyY)) {
        Serial.println("BLE: Failed to set device public key");
        _state = ConnectionState::Error;
        return;
    }

    // Update state
    _state = ConnectionState::KeyExchanged;
    if (debugEnabled) {
        Serial.println("BLE: Key exchange notification processed");
    }
}

void CasambiClient::_handleAuthNotification(uint8_t* data, size_t len) {
    if (!_encryption) {
        Serial.println("BLE: Encryption not initialized");
        _state = ConnectionState::Error;
        return;
    }

    // Auth response is encrypted packet: [counter:4][type:1][data...]
    if (len < CMAC_SIZE + 5) {
        Serial.printf("BLE: Auth response too short: %d\n", len);
        _state = ConnectionState::Error;
        return;
    }

    // Extract counter from first 4 bytes
    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    if (debugEnabled) {
        Serial.printf("BLE: Auth response packet (counter=%u, len=%d)\n", counter, len);
        Serial.printf("BLE: First bytes: %02x %02x %02x %02x %02x\n",
            data[0], data[1], data[2], data[3], data[4]);
    }

    // For INCOMING packets, nonce is: [packet_counter(4)] + [device_nonce(4-15)]
    // This is different from outgoing packets!
    std::vector<uint8_t> nonce(NONCE_SIZE);
    // First 4 bytes: counter from packet (including direction bit!)
    memcpy(nonce.data(), data, 4);
    // Next 12 bytes: device nonce bytes 4-15
    memcpy(nonce.data() + 4, _nonce + 4, 12);

    if (debugEnabled) {
        Serial.printf("BLE: Packet counter=0x%08x\n", counter);
        Serial.print("BLE: Nonce: ");
        for (int i = 0; i < 16; i++) Serial.printf("%02x ", nonce[i]);
        Serial.println();
    }

    // Decrypt and verify (header=4 bytes for counter)
    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);

    if (debugEnabled) {
        Serial.print("BLE: Encrypted payload: ");
        for (int i = 4; i < 9 && i < len; i++) Serial.printf("%02x ", data[i]);
        Serial.println();
        if (plaintext.size() > 0) {
            Serial.print("BLE: Decrypted: ");
            for (int i = 0; i < 5 && i < plaintext.size(); i++) Serial.printf("%02x ", plaintext[i]);
            Serial.println();
        }
    }

    if (plaintext.size() == 0) {
        Serial.println("BLE: Auth response decryption failed");
        _state = ConnectionState::Error;
        return;
    }

    // Check response type
    uint8_t responseType = plaintext[0];

    if (responseType == 0x05) {
        if (debugEnabled) {
            Serial.println("BLE: Authentication successful!");
        }
        _state = ConnectionState::Authenticated;
    } else if (responseType == 0x06) {
        Serial.println("BLE: Authentication rejected by device");
        _state = ConnectionState::Error;
    } else {
        Serial.printf("BLE: Unexpected auth response type: 0x%02x\n", responseType);
        _state = ConnectionState::Error;
    }
}

void CasambiClient::_handleDataNotification(uint8_t* data, size_t len) {
    if (!_encryption) {
        Serial.println("BLE: Encryption not initialized");
        return;
    }

    // Encrypted data packet: [counter:4][type:1][data...]
    if (len < CMAC_SIZE + 5) {
        Serial.printf("BLE: Data packet too short: %d\n", len);
        return;
    }

    // Extract counter from first 4 bytes
    uint32_t counter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    // For INCOMING packets, nonce is: [packet_counter(4)] + [device_nonce(4-15)]
    std::vector<uint8_t> nonce(NONCE_SIZE);
    memcpy(nonce.data(), data, 4);  // Counter from packet
    memcpy(nonce.data() + 4, _nonce + 4, 12);  // Device nonce bytes 4-15

    // Decrypt and verify (header=4 bytes for counter)
    std::vector<uint8_t> packet(data, data + len);
    std::vector<uint8_t> plaintext = _encryption->decryptAndVerify(packet, nonce, 4);

    if (plaintext.size() == 0) {
        Serial.println("BLE: Data packet decryption failed");
        return;
    }

    // Parse packet type
    uint8_t packetType = plaintext[0];

    if (debugEnabled) {
        Serial.printf("BLE: Data packet type=0x%02x, len=%d, counter=0x%08x\n", packetType, plaintext.size(), counter);
    }

    // Handle different packet types
    switch (packetType) {
        case 0x08:
            // Unit state update
            if (debugEnabled) {
                Serial.println("BLE: Unit state update received");
            }
            // TODO: Parse unit state and update NetworkConfig
            break;

        case 0x09:
            // Network state
            if (debugEnabled) {
                Serial.println("BLE: Network state received");
            }
            break;

        default:
            if (debugEnabled) {
                Serial.printf("BLE: Unknown data packet type: 0x%02x\n", packetType);
            }
            break;
    }
}
