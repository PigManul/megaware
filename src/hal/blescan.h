#pragma once
#include <Arduino.h>

// =====================================================================
//  BleScan — shared wrapper over the Bluedroid scanner for Bluetooth
//  features. IMPORTANT (memory): the BLE stack eats ~60-70 KB of
//  internal heap; it is best to scan with Wi-Fi off (Settings -> Wi-Fi ->
//  Release Wi-Fi RAM). Features warn on their own if memory is low.
// =====================================================================

namespace BleScan {
    struct Entry {
        char addr[18];
        char name[24];
        int  rssi;
        bool findMy; // Apple Find My / AirTag-like advertising
    };

    // Blocking scan (seconds). false — BLE did not come up (low heap).
    bool scanBlocking(int seconds);
    int  count();
    const Entry& at(int i);
    int  findMyCount();
    // Tear down Bluedroid entirely: returns ~50-60 KB of heap.
    // The next scanBlocking() will bring the stack back up (~1 s).
    void deinit();
}
