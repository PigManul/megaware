// =====================================================================
//  Sub-GHz: Decoder — analysis of raw timings from the last capture.
//  Recognizes fixed protocols (Princeton/EV1527, CAME, Nice) by pulse
//  width patterns. Works on the data captured by Capture.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"

namespace {
    struct DecodeResult {
        char proto[16];
        uint32_t bits;          // decoded bits
        int bitCount;
        uint32_t te;            // base pulse, us
        bool valid;
    };

    DecodeResult result = {};
    bool hasResult = false;
    uint32_t lastCaptureCount = 0;

    // Attempt to decode Princeton/EV1527: SYNC (HIGH ~TE*32, LOW ~TE),
    // then 24 bits: each = HIGH TE + LOW 3*TE (0) or HIGH 3*TE + LOW TE (1)
    bool tryPrinceton(const RawCapture& cap, DecodeResult& r) {
        if (cap.count < 26) return false;
        // find sync: the longest LOW (odd index, negative)
        uint16_t maxLow = 0;
        int syncIdx = -1;
        for (int i = 1; i < (int)cap.count - 48; i += 2) {
            if (cap.durationsUs[i] > maxLow) { maxLow = cap.durationsUs[i]; syncIdx = i; }
        }
        if (maxLow < 3000 || syncIdx < 0) return false; // sync gap > 3ms
        int te = 0;
        // PT from the first pulses after sync
        if (syncIdx + 4 > (int)cap.count) return false;
        te = cap.durationsUs[syncIdx + 1] / 3;
        if (te < 100 || te > 1000) return false;
        // count bits: 24 bits, 2 pulses each
        int bits = 0;
        uint32_t data = 0;
        for (int i = 0; i < 24; i++) {
            int hi = syncIdx + 1 + i * 2;
            int lo = hi + 1;
            if (lo >= (int)cap.count) return false;
            uint16_t h = cap.durationsUs[hi], l = cap.durationsUs[lo];
            if (h > l * 2) data = (data << 1) | 1; // long-high = 1
            else if (l > h * 2) data = (data << 1); // long-low = 0
            else return false;
            bits++;
        }
        if (bits != 24) return false;
        snprintf(r.proto, sizeof(r.proto), "Princeton");
        r.bits = data;
        r.bitCount = 24;
        r.te = te;
        r.valid = true;
        return true;
    }

    // CAME 12-bit: similar, but only 12 bits
    bool tryCame(const RawCapture& cap, DecodeResult& r) {
        if (cap.count < 26) return false;
        uint16_t maxLow = 0;
        int syncIdx = -1;
        for (int i = 1; i < (int)cap.count - 26; i += 2) {
            if (cap.durationsUs[i] > maxLow) { maxLow = cap.durationsUs[i]; syncIdx = i; }
        }
        if (maxLow < 2000 || syncIdx < 0) return false;
        int te = cap.durationsUs[syncIdx + 1] / 2;
        if (te < 100 || te > 800) return false;
        int bits = 0;
        uint32_t data = 0;
        for (int i = 0; i < 12; i++) {
            int hi = syncIdx + 1 + i * 2;
            int lo = hi + 1;
            if (lo >= (int)cap.count) return false;
            uint16_t h = cap.durationsUs[hi], l = cap.durationsUs[lo];
            if (h > l * 2) data = (data << 1) | 1;
            else if (l > h * 2) data = (data << 1);
            else return false;
            bits++;
        }
        if (bits != 12) return false;
        snprintf(r.proto, sizeof(r.proto), "CAME");
        r.bits = data;
        r.bitCount = 12;
        r.te = te;
        r.valid = true;
        return true;
    }

    void decode() {
        memset(&result, 0, sizeof(result));
        result.valid = false;
        hasResult = false;
        RawCapture& cap = SubGhz::sharedCapture();
        if (cap.count < 20 || !cap.valid) return;

        if (tryPrinceton(cap, result)) { hasResult = true; return; }
        if (tryCame(cap, result)) { hasResult = true; return; }
        // unknown: show statistics
        snprintf(result.proto, sizeof(result.proto), "Unknown");
        result.bitCount = cap.count;
        result.valid = true;
        hasResult = true;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PROTOCOL DECODER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        RawCapture& cap = SubGhz::sharedCapture();
        if (!cap.valid) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("capture a signal first", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("SubGhz -> Capture -> then open", x0 + 10, y0 + 44);
            return;
        }

        if (!hasResult) {
            decode();
            // decode on open
        }

        if (!hasResult) return;

        if (strcmp(result.proto, "Unknown") == 0) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("Unknown protocol", x0 + 10, y0 + 26);
            char line[48];
            snprintf(line, sizeof(line), "%d edges captured", (int)cap.count);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 44);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("pattern not recognized", x0 + 10, y0 + 62);
            return;
        }

        c.setTextColor(Theme::OK, Theme::BG);
        c.drawString(result.proto, x0 + 10, y0 + 26);
        char line[56];
        snprintf(line, sizeof(line), "Data:  0x%06lX", (unsigned long)result.bits);
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 44);
        snprintf(line, sizeof(line), "Bits:  %d", result.bitCount);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 62);
        snprintf(line, sizeof(line), "TE:    %lu us", (unsigned long)result.te);
        c.drawString(line, x0 + 10, y0 + 80);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rotate: rescan   click: replay", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            SubGhz::replayRaw(SubGhz::sharedCapture());
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!hasResult && SubGhz::sharedCapture().valid) {
            decode();
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Decoder", MwCat::SubGhz, MwIcon::Chip, 50,
                 nullptr, input, tick, draw, nullptr);
