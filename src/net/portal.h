#pragma once
#include <Arduino.h>

// =====================================================================
//  Portal — Captive Portal with the OWNER'S OWN page.
//
//  Neutral implementation: the device raises an AP and a connected
//  client, via captive-detection, gets the selected page —
//  /portal/*.html from SD/Flash (uploaded via WebUI) or the built-in
//  stub. This is the owner's landing page — greeting, instructions,
//  demo. No phishing templates (fake password pages) in the firmware.
//
//  Resources: takes :80 and :53 via ResMgr — WebUI steps off the ports
//  and returns after Portal::stop(). The AP is raised automatically.
//  Runs in the background: Portal::loop() is called from the main tick.
// =====================================================================

namespace Portal {
    bool start(const String& pagePath); // "" = built-in page
    void stop();
    bool isRunning();
    void loop();                        // serve clients (from the main tick)
    uint32_t hits();
    const char* pageName();             // currently selected page (for the UI)
}
