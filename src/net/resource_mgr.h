#pragma once
#include <Arduino.h>

// =====================================================================
//  ResMgr — arbiter of network resources between WebUI and features.
//
//  WebUI is NOT the owner of the ports — it is a consumer just like
//  any feature. A feature (captive portal, its own HTTP/DNS server,
//  REST) claims the resource before working and releases it when done:
//
//    ResMgr::claim(ResMgr::Resource::Http80, "my_feature");   // onEnter
//    ResMgr::release(ResMgr::Resource::Http80, "my_feature"); // onExit
//
//  WebUI watches the claims in WebUI::loop() and steps aside on its
//  own (stops its WebServer/DNSServer) and comes back by itself once
//  the resource is free. While a resource is claimed, WebUI::isRunning()
//  honestly reports false — "enabled, but preempted".
//
//  No conflicts (no arbitration needed): outbound internet from features
//  (HTTPClient, MQTT, WiFiClient) and AP mode itself (AP+STA is shared).
// =====================================================================

namespace ResMgr {
    enum class Resource : uint8_t {
        Http80, // TCP :80 — WebUI WebServer / feature's own HTTP server
        Dns53,  // UDP :53 — WebUI DNSServer / feature's own DNS (captive)
    };

    // true — the resource is yours (or already was). false — held by another feature.
    bool claim(Resource r, const char* owner);
    // Idempotent; with a foreign owner it is a no-op.
    void release(Resource r, const char* owner);
    // Whether anyone holds it (WebUI steps off the resource on this flag).
    bool isHeld(Resource r);
}
