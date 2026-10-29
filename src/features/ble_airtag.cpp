// =====================================================================
//  Bluetooth: Trackers — detector of Apple Find My trackers (AirTag and
//  similar) near the device. Passive reception of advertising.
//  Cycle: a 2 s scan every ~6 s; if a foreign tracker is in range — the LED
//  goes red (Board::setLedMode(Solid) red until the tracker leaves).
//  Quit — exit (the LED returns to the user's settings).
// =====================================================================
#include "../ui/features.h"
#include "../hal/blescan.h"
#include "../../include/theme.h"

namespace {
    uint32_t nextScanAt = 0;
    int seenCount = 0;
    int bestRssi = -128;
    char bestName[24] = "";
    bool scanBusy = false;
    bool wasLedTouched = false;

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TRACKER DETECTOR");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (ESP.getFreeHeap() < 70000) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("low heap — release Wi-Fi RAM", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("(Settings -> Wi-Fi), Quit and reopen", x0 + 10, y0 + 40);
            return;
        }

        if (seenCount > 0) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("TRACKER IN FIELD", x0 + 10, y0 + 26);
            char line[56];
            snprintf(line, sizeof(line), "%d find-my device(s)", seenCount);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 46);
            snprintf(line, sizeof(line), "strongest %d dBm %s", bestRssi, bestName);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 62);
        } else {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString(scanBusy ? "SCANNING..." : "No trackers nearby", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("passive scan every ~6 s", x0 + 10, y0 + 46);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("detection: Apple Find My adv", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        if (scanBusy) return;
        if (millis() < nextScanAt) return;
        scanBusy = true;
        bool ok = BleScan::scanBlocking(2);
        scanBusy = false;
        nextScanAt = millis() + 4000;
        if (!ok) return;

        seenCount = BleScan::findMyCount();
        bestRssi = -128;
        bestName[0] = 0;
        for (int i = 0; i < BleScan::count(); i++) {
            const BleScan::Entry& e = BleScan::at(i);
            if (e.findMy && e.rssi > bestRssi) {
                bestRssi = e.rssi;
                snprintf(bestName, sizeof(bestName), "%s", e.name[0] ? e.name : e.addr);
            }
        }
        BleScan::deinit(); // between passes the stack is unloaded — the heap is not held
        if (seenCount > 0 && !wasLedTouched) {
            Board::setLedColor(0xFF, 0x50, 0x50);
            Board::setLedMode(LedMode::Solid);
            wasLedTouched = true;
        } else if (seenCount == 0 && wasLedTouched) {
            Board::setLedColor(0x39, 0xFF, 0x88);
            Board::setLedMode(LedMode::Breathing);
            wasLedTouched = false;
        }
        Features::requestRedraw();
    }

    void onExit() {
        if (wasLedTouched) {
            Board::setLedMode(LedMode::Breathing); // restore the user color
            wasLedTouched = false;
        }
    }
}

REGISTER_FEATURE("Trackers", "Others", MwIcon::Lock, 20,
                 nullptr, input, tick, draw, onExit);
