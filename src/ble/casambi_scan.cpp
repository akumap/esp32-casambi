/**
 * Casambi BLE Scan helper - implementation
 */

#include "casambi_scan.h"
#include "../config.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

namespace {

String toHex(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    String out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += hex[c >> 4];
        out += hex[c & 0x0F];
    }
    return out;
}

// Collects matching Casambi advertisers into a caller-supplied vector.
class Collector : public BLEAdvertisedDeviceCallbacks {
public:
    std::vector<CasambiScanResult>* out = nullptr;

    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveServiceUUID()) return;
        if (!dev.getServiceUUID().equals(BLEUUID(CASAMBI_SERVICE_UUID))) return;

        String mac = dev.getAddress().toString().c_str();
        for (const auto& r : *out) {
            if (r.mac == mac) return;  // already seen
        }

        CasambiScanResult r;
        r.mac  = mac;
        r.uuid = mac;
        r.uuid.replace(":", "");
        r.uuid.toLowerCase();
        r.name = dev.haveName() ? String(dev.getName().c_str()) : String("");
        r.rssi = dev.getRSSI();
        if (dev.haveManufacturerData()) r.mfgData = toHex(dev.getManufacturerData());
        if (dev.haveServiceData())      r.svcData = toHex(dev.getServiceData());

        out->push_back(r);
    }
};

} // namespace

void CasambiScan::run(uint32_t seconds, std::vector<CasambiScanResult>& out) {
    out.clear();

    BLEScan* scan = BLEDevice::getScan();
    Collector cb;
    cb.out = &out;

    scan->setAdvertisedDeviceCallbacks(&cb);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);

    scan->start(seconds, false);   // blocking
    scan->clearResults();

    // cb lives on the stack; detach before it goes out of scope.
    scan->setAdvertisedDeviceCallbacks(nullptr);
}
