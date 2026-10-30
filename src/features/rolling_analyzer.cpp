// =====================================================================
//  Sub-GHz: Rolling Code — passive transmitter analyzer.
//  Captures OOK bursts one by one, folds each into bits (same PPM
//  rules as RF Workbench), groups by (bit length, address = top bits)
//  and tracks whether the payload changes between transmissions:
//  fixed code -> same key every time, hopping code -> key counter
//  changes (the delta row shows XOR distance to the previous key).
//  Pure RX analysis: nothing is transmitted, nothing predicted.
//  Click: capture next burst. Long press: clear table.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"

namespace {
    struct TxRec {
        uint32_t bitsLen;
        uint32_t addr;      // top 24 bits of the frame
        uint32_t keysSeen;
        uint32_t lastKey;   // low 24 bits
        uint32_t lastKeyPrev;
        uint32_t keyChanges;
        uint32_t firstSeenMs;
        uint32_t lastSeenMs;
    };
    constexpr int MAX_TX = 10;
    TxRec recs[MAX_TX];
    int recCount = 0;

    char statusMsg[48] = "";
    bool capturing = false;
    RawCapture cap;

    bool decodeBits(RawCapture& c, uint8_t* out, int maxBits, int& bitCount, uint32_t& unit) {
        uint32_t minUs = 0xFFFFFFFF;
        for (size_t i = 0; i < c.count; i++)
            if (c.durationsUs[i] < minUs && c.durationsUs[i] >= 40) minUs = c.durationsUs[i];
        if (minUs == 0xFFFFFFFF) return false;
        unit = minUs;
        static uint8_t cells[2048];
        int cellsN = 0;
        bool level = true;
        for (size_t i = 0; i < c.count && cellsN < 2048; i++) {
            uint32_t u = (c.durationsUs[i] + unit / 2) / unit;
            if (u < 1) u = 1;
            for (uint32_t k = 0; k < u && cellsN < 2048; k++) cells[cellsN++] = level;
            level = !level;
        }
        bitCount = 0;
        for (int i = 0; i + 1 < cellsN && bitCount < maxBits; i += 2) {
            uint8_t b;
            if (cells[i] == 1 && cells[i + 1] == 0) b = 0;
            else if (cells[i] == 0 && cells[i + 1] == 1) b = 1;
            else b = cells[i];
            if (bitCount % 8 == 0) out[bitCount / 8] = 0;
            if (b) out[bitCount / 8] |= (0x80 >> (bitCount % 8));
            bitCount++;
        }
        return bitCount >= 12;
    }

    uint32_t extractBits(const uint8_t* bits, int bitCount, int from, int len) {
        uint32_t v = 0;
        for (int i = 0; i < len && from + i < bitCount; i++) {
            v <<= 1;
            if (bits[(from + i) / 8] & (0x80 >> ((from + i) % 8))) v |= 1;
        }
        return v;
    }

    void addCapture() {
        if (!cap.valid || cap.count < 8) { snprintf(statusMsg, sizeof(statusMsg), "empty burst"); return; }
        static uint8_t bits[96];
        int bitCount = 0;
        uint32_t unit = 0;
        if (!decodeBits(cap, bits, 96 * 8, bitCount, unit)) {
            snprintf(statusMsg, sizeof(statusMsg), "decode failed (%lu edges)", (unsigned long)cap.count);
            return;
        }
        uint32_t addr = extractBits(bits, bitCount, 0, 24);
        uint32_t key = extractBits(bits, bitCount, bitCount > 24 ? 24 : 0, 24);
        TxRec* r = nullptr;
        for (int i = 0; i < recCount; i++)
            if (recs[i].bitsLen == (uint32_t)bitCount && recs[i].addr == addr) { r = &recs[i]; break; }
        if (!r) {
            if (recCount >= MAX_TX) { snprintf(statusMsg, sizeof(statusMsg), "table full (%d tx)", MAX_TX); return; }
            r = &recs[recCount++];
            memset(r, 0, sizeof(*r));
            r->bitsLen = bitCount;
            r->addr = addr;
            r->firstSeenMs = millis();
        }
        if (r->keysSeen > 0 && key != r->lastKey) r->keyChanges++;
        r->lastKeyPrev = r->lastKey;
        r->lastKey = key;
        r->keysSeen++;
        r->lastSeenMs = millis();
        snprintf(statusMsg, sizeof(statusMsg), "tx: %d bits, addr %06lX", bitCount, (unsigned long)addr);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "ROLLING CODE ANALYZER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("passive RX only - no transmit, no prediction", x0 + 10, y0 + 22);

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("bits  address  keys  hop  last key", x0 + 10, y0 + 40);
        for (int i = 0; i < recCount; i++) {
            int y = y0 + 56 + i * Theme::ROW_LIST;
            if (y > y0 + h - 34) break;
            bool hop = recs[i].keysSeen > 1 && recs[i].keyChanges > 0;
            snprintf(line, sizeof(line), "%3lu   %06lX   %3lu   %s  %06lX",
                     (unsigned long)recs[i].bitsLen, (unsigned long)recs[i].addr,
                     (unsigned long)recs[i].keysSeen, hop ? "yes" : "no ",
                     (unsigned long)recs[i].lastKey);
            c.setTextColor(hop ? Theme::WARN : Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 10, y);
        }
        if (recCount == 0) {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click to grab a burst from the remote", x0 + 10, y0 + 60);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 32);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: grab burst   hold: clear", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (capturing || !SubGhz::isReady()) return true;
            capturing = true;
            SubGhz::startRawCapture(&cap, 2000);
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            recCount = 0;
            statusMsg[0] = 0;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (capturing && !SubGhz::isCapturing()) {
            SubGhz::stopRawCapture();
            capturing = false;
            addCapture();
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Rolling Code", MwCat::SubGhz, MwIcon::Lock, 30,
                 nullptr, input, tick, draw, nullptr);
