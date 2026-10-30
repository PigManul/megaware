#pragma once
#include <Arduino.h>

// =====================================================================
//  BridgeCfg — TCP bridge port-forward rules, shared between the
//  Ethernet bridge feature and the WebUI terminal commands.
//  Each rule: a board listening port accepted on ANY interface
//  (cable or Wi-Fi), relayed byte-by-byte to host:rport over the
//  Wi-Fi uplink. Persisted to NVS.
// =====================================================================

struct BridgeRule {
    bool enabled = false;
    uint16_t lport = 0;
    uint16_t rport = 0;
    char host[40] = "";
};

namespace BridgeCfg {
    constexpr int SLOTS = 2;

    void load(BridgeRule out[SLOTS]);
    void save(int slot, const BridgeRule& r);
    // increments on every save - the feature watches it to re-apply rules
    uint32_t version();
}
