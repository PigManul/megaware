// =====================================================================
//  Sub-GHz: Scan — live RSSI indicator of the airwaves.
//  Migrated from Ui::drawSubGhz (tab 1) into the feature registry with no visual changes.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"

namespace {
    float smoothRssi = -100.0f;

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "LIVE RSSI");

        bool ready = SubGhz::isReady();
        float rssi = ready ? SubGhz::readRssiDbm() : -100.0f;
        smoothRssi = smoothRssi * 0.85f + rssi * 0.15f;
        if (fabsf(rssi - smoothRssi) > 0.5f) Features::requestRedraw(); // was frameMoving

        float pct = constrain((smoothRssi + 100.0f) / 70.0f, 0.0f, 1.0f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", smoothRssi);
        MwDraw::gauge(c, x0 + w / 2, y0 + h / 2, 32, pct, buf, "dBm");
    }
}

REGISTER_FEATURE("Scan", MwCat::SubGhz, MwIcon::None, 1,
                 nullptr, nullptr, nullptr, draw, nullptr);
