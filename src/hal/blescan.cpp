#include "blescan.h"
#include <BLEDevice.h>

namespace {
    bool inited = false;
    BleScan::Entry entries[40];
    int entryCount = 0;
    int findMyN = 0;

    bool looksLikeFindMy(const std::string& md) {
        // Apple manufacturer data (0x004C) with an offline-finding payload (type 0x12):
        // this is how AirTag/Find My trackers and Apple "offline" devices advertise.
        return md.size() >= 0x1B &&
               (uint8_t)md[0] == 0x4C && (uint8_t)md[1] == 0x00 &&
               (uint8_t)md[2] == 0x12;
    }
}

namespace BleScan {

bool scanBlocking(int seconds) {
    if (!inited) {
        // name shared with BLE Typer: the Bluedroid stack is initialized once,
        // and whichever init() comes first sets the device name on air
        BLEDevice::init("MEGAWARE");
        inited = true;
    }
    BLEScan* scan = BLEDevice::getScan();
    if (!scan) return false;
    scan->setActiveScan(true);
    BLEScanResults res = scan->start(seconds, false);

    entryCount = 0;
    findMyN = 0;
    int n = res.getCount();
    if (n > 40) n = 40;
    for (int i = 0; i < n; i++) {
        BLEAdvertisedDevice d = res.getDevice(i);
        Entry& e = entries[entryCount];
        memset(e.addr, 0, sizeof(e.addr));
        memset(e.name, 0, sizeof(e.name));
        snprintf(e.addr, sizeof(e.addr), "%s", d.getAddress().toString().c_str());
        if (d.haveName()) snprintf(e.name, sizeof(e.name), "%s", d.getName().c_str());
        e.rssi = d.getRSSI();
        e.findMy = looksLikeFindMy(d.getManufacturerData());
        if (e.findMy) findMyN++;
        entryCount++;
    }
    // insertion sort by RSSI (descending)
    for (int i = 1; i < entryCount; i++) {
        Entry t = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].rssi < t.rssi) { entries[j + 1] = entries[j]; j--; }
        entries[j + 1] = t;
    }
    scan->clearResults();
    return true;
}

int count() { return entryCount; }
const Entry& at(int i) { return entries[i]; }
int findMyCount() { return findMyN; }

void deinit() {
    if (!inited) return;
    // results are already in the static array — the stack can be torn down
    BLEDevice::deinit(true);
    inited = false;
}

} // namespace BleScan
