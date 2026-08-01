/**
 * Casambi BLE Scan helper
 *
 * Self-contained BLE discovery used by the setup portal (and available to the
 * serial path). Collects every device advertising the Casambi service UUID,
 * including manufacturer/service data for disambiguation between networks.
 *
 * The caller is responsible for BLEDevice::init()/deinit() around run().
 */

#ifndef CASAMBI_SCAN_H
#define CASAMBI_SCAN_H

#include <Arduino.h>
#include <vector>

struct CasambiScanResult {
    String  uuid;     // BLE MAC without colons, lowercase → networkUuid candidate
    String  mac;      // BLE MAC with colons (as advertised)
    String  name;     // Advertised "Complete Local Name" (often the network name)
    int     rssi;
    String  mfgData;  // Manufacturer data (hex), empty if absent
    String  svcData;  // Service data (hex), empty if absent
    // Advertised BLE address type (BLE_ADDR_PUBLIC/RANDOM/…). The client
    // reconnects by MAC with a hard-coded PUBLIC type, so a peer advertising a
    // RANDOM address can be scanned but never connected — that mismatch is
    // invisible without this field (see addrTypeName()).
    uint8_t addrType;

    CasambiScanResult() : rssi(0), addrType(0) {}
};

namespace CasambiScan {
    /**
     * Run a blocking BLE scan for the given number of seconds and fill `out`
     * with the discovered Casambi devices (deduplicated by MAC).
     * Requires BLEDevice::init() to have been called beforehand.
     */
    void run(uint32_t seconds, std::vector<CasambiScanResult>& out);

    /**
     * Human-readable BLE address type ("public", "random", "public-id",
     * "random-id"). Static string, never nullptr — usable in any trace.
     */
    const char* addrTypeName(uint8_t type);
}

#endif // CASAMBI_SCAN_H
