#pragma once
#include <Arduino.h>
#include <WiFi.h>

struct WifiNetwork {
    String ssid;
    int32_t rssi;
    int32_t channel;
    bool secure;
};

enum class WifiConnState { Idle, Connecting, Connected, Failed };

namespace WifiMgr {
    void begin();

    void startScan();
    bool isScanning();
    int  resultCount();
    WifiNetwork result(int index);

    void connect(const String& ssid, const String& password);
    void disconnect();
    void releaseIdleResources();
    WifiConnState connState();
    String currentSSID();
    int rssiPercent(int32_t rssiDbm);

    bool isConnected();

    void startAP(int preset); // 0=192.168.4.1 1=192.168.1.1 2=172.16.0.1
    void stopAP();
    bool isAPRunning();
    String apIP();
    const char* hostname();

    // Tries to auto-connect to the network saved via Config
    // (see src/hal/config.h) — call once from setup(). If no network
    // is saved, the radio is not touched at all (saves memory).
    void tryAutoConnect();
    // Call from the main loop: retries auto-connect after failures
    // (the network may not be visible right after the radio comes up).
    void serviceAutoConnect();
}
