// =====================================================================
//  Sub-GHz: RF Workbench — full on-device protocol dissector.
//  Capture a burst (raw edges from CC1101 GDO2), the Dissect engine
//  (src/radio/dissect.cpp) autodetects the encoding (PWM/PPM vs
//  Manchester vs NRZ), extracts preamble/sync/timing and matches the
//  frame against common remotes: CAME/Nice 12-bit, Princeton PT-2262
//  with tri-state digits; unknown frames still get a full field view.
//  Plus a pulse-width histogram and raw .sub export for offline work.
//  Click: capture. Long press: save raw to /subghz/wb_<ms>.sub.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../radio/dissect.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State { Idle, Capturing, Analyzed };
    State state = Idle;

    RawCapture cap;
    DissectResult dis;
    bool dewhit = false;      // PN9 de-scrambling toggle (rotation)
    int hist[6] = {0};        // bucket counts: ~1u, 2u, 3u..4u, 5u..8u, 9u..16u, >16u
    char statusMsg[48] = "";

    void analyze() {
        statusMsg[0] = 0;
        for (int i = 0; i < 6; i++) hist[i] = 0;
        if (!cap.valid || cap.count < 8) {
            snprintf(statusMsg, sizeof(statusMsg), "too few edges (%lu)", (unsigned long)cap.count);
            return;
        }
        uint32_t minUs = 0xFFFFFFFF;
        for (size_t i = 0; i < cap.count; i++)
            if (cap.durationsUs[i] < minUs && cap.durationsUs[i] >= 40) minUs = cap.durationsUs[i];
        if (minUs == 0xFFFFFFFF) { snprintf(statusMsg, sizeof(statusMsg), "no usable pulses"); return; }

        for (size_t i = 0; i < cap.count; i++) {
            uint32_t u = (cap.durationsUs[i] + minUs / 2) / minUs;
            if (u <= 1) hist[0]++;
            else if (u == 2) hist[1]++;
            else if (u <= 4) hist[2]++;
            else if (u <= 8) hist[3]++;
            else if (u <= 16) hist[4]++;
            else hist[5]++;
        }
        Dissect::analyze(cap, dis);
        if (dewhit && dis.valid && dis.bitCount >= 16) Dissect::dewhitenPn9(dis);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "RF WORKBENCH");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "RX %.2f MHz (set in Scan/Capture)", SubGhz::currentFrequencyMHz());
        c.drawString(line, x0 + 10, y0 + 22);

        if (state == Idle) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: capture burst", x0 + 10, y0 + 22);
        } else if (state == Capturing) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("capturing (GDO2 edges)...", x0 + 10, y0 + 22);
        } else {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            snprintf(line, sizeof(line), "%s  [%s]", dis.proto, dis.encName);
            c.drawString(line, x0 + 10, y0 + 22);
            c.setTextColor(Theme::TEXT, Theme::BG);
            int slen = strlen(dis.summary);
            char s1[53], s2[53]; // no String per frame
            snprintf(s1, sizeof(s1), "%s", dis.summary);
            c.drawString(s1, x0 + 10, y0 + 34);
            if (slen > 52) {
                snprintf(s2, sizeof(s2), "%s", dis.summary + 52);
                c.drawString(s2, x0 + 10, y0 + 44);
            }

            // pulse histogram (6 buckets)
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("pulses: 1u 2u 3-4u 5-8u 9-16u >16u", x0 + 10, y0 + 58);
            int maxH = 1;
            for (int i = 0; i < 6; i++) if (hist[i] > maxH) maxH = hist[i];
            for (int i = 0; i < 6; i++) {
                int bh = hist[i] * 22 / maxH;
                c.fillRect(x0 + 10 + i * 22, y0 + 84 - bh, 16, bh, Theme::ACCENT);
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
                snprintf(line, sizeof(line), "%lu", (unsigned long)hist[i]);
                c.drawString(line, x0 + 10 + i * 22, y0 + 86);
            }

            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            snprintf(line, sizeof(line), "unit %lu us  ~%lu us/cell  edges %lu",
                     (unsigned long)dis.unitUs, (unsigned long)dis.baud, (unsigned long)cap.count);
            c.drawString(line, x0 + 10, y0 + 100);
            c.setTextColor(dewhit ? Theme::OK : Theme::TEXT_FAINT, Theme::BG);
            snprintf(line, sizeof(line), "PN9 de-whiten: %s (rot toggles)", dewhit ? "ON" : "off");
            c.drawString(line, x0 + 10, y0 + 110);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 44);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: capture   hold: save .sub", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0 && state == Analyzed) {
            // rotation toggles PN9 de-whitening and re-decodes in place
            dewhit = !dewhit;
            analyze();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (state != Capturing && SubGhz::isReady()) {
                state = Capturing;
                SubGhz::startRawCapture(&cap, 3000);
                Features::requestRedraw();
            }
            return true;
        }
        if (in.event == InputEvent::LongPress && state == Analyzed && cap.valid) {
            String path = "/subghz/wb_" + String(millis()) + ".sub";
            if (!SD.exists("/subghz") && !LittleFS.exists("/subghz")) SD.mkdir("/subghz");
            bool ok = SubGhz::saveRaw(path, cap);
            snprintf(statusMsg, sizeof(statusMsg), ok ? "saved %s" : "save failed", ok ? path.c_str() : "");
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state == Capturing) {
            if (!SubGhz::isCapturing()) {
                SubGhz::stopRawCapture();
                analyze();
                state = Analyzed;
                Features::requestRedraw();
            }
        }
    }
}

REGISTER_FEATURE("RF Workbench", MwCat::SubGhz, MwIcon::Chip, 20,
                 nullptr, input, tick, draw, nullptr);
