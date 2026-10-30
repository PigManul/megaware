// =====================================================================
//  Sub-GHz: POCSAG RX — pager network monitor (receive only!).
//  POCSAG: 512/1200/2400 bps 2-FSK, 32-bit BCH(31,21) codewords,
//  sync word 0x7CD215D8. We sample the CC1101 async data output at
//  1200 bps for ~1.1 s per tick, search for the sync word and decode
//  address + numeric (BCD) / alphanumeric (7-bit) messages.
//  Rotation: frequency preset. Click: start/stop monitoring.
//  TX on pager frequencies is licensed spectrum - not implemented.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

namespace {
    bool monitoring = false;
    int preset = 0;
    static const float FREQS[4] = {466.230f, 439.9875f, 452.025f, 433.500f};

    uint32_t syncFound = 0;
    uint32_t msgCount = 0;
    struct Msg { uint32_t addr; char text[37]; };
    Msg msgs[4];
    int msgHead = 0;
    char statusMsg[48] = "";

    constexpr int BITS = 1760; // ~1.47 s at 1200 bps
    uint8_t rawBits[BITS / 8];
    int rawN = 0;

    void setupFsk() {
        if (!SubGhz::isReady()) return;
        ELECHOUSE_cc1101.setCCMode(0);
        ELECHOUSE_cc1101.setModulation(0); // 2-FSK
        ELECHOUSE_cc1101.setDeviation(4.5f);
        ELECHOUSE_cc1101.setDRate(1.2f);
        ELECHOUSE_cc1101.setRxBW(58.0f);
        ELECHOUSE_cc1101.setPA(10);
        SubGhz::setFrequency(FREQS[preset]);
        ELECHOUSE_cc1101.SetRx();
    }

    void restoreDefaults() {
        if (!SubGhz::isReady()) return;
        ELECHOUSE_cc1101.setCCMode(0);
        ELECHOUSE_cc1101.setModulation(2);
        ELECHOUSE_cc1101.setPA(10);
        SubGhz::setFrequency(SubGhz::currentFrequencyMHz());
        ELECHOUSE_cc1101.SetRx();
    }

    bool bchOk(uint32_t cw31) { // generator 0x769, MSB-first division
        for (int i = 30; i >= 10; i--)
            if (cw31 & (1UL << i)) cw31 ^= 0x769UL << (i - 10);
        return cw31 == 0;
    }

    bool bitAt(int i) { return rawBits[i >> 3] & (0x80 >> (i & 7)); }

    uint32_t takeBits(int from, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) { v <<= 1; if (bitAt(from + i)) v |= 1; }
        return v;
    }

    // find 0x7CD215D8 in the bit stream, decode batches after it
    void decode() {
        int pos = 0;
        while (pos + 32 <= rawN) {
            uint32_t cw = takeBits(pos, 32);
            if (cw != 0x7CD215D8UL) { pos++; continue; }
            syncFound++;
            int cwOff = pos + 32;
            uint32_t addr = 0;
            char text[37]; text[0] = 0;
            int textN = 0;
            uint8_t msgBits[120]; int mbN = 0;
            bool haveAddr = false;
            for (int b = 0; b < 34 && cwOff + 32 <= rawN; b++, cwOff += 32) {
                uint32_t c = takeBits(cwOff, 32);
                bool parity = false;
                for (int i = 0; i < 32; i++) if (c & (1UL << i)) parity = !parity;
                if (!bchOk(c >> 1) || parity) continue; // drop uncorrectable
                if (!(c & 0x80000000UL)) { // address codeword
                    if (haveAddr && mbN > 0) break; // new batch address, flush
                    addr = ((c >> 13) & 0x3FFFF) << 3 | ((c >> 11) & 0x3);
                    haveAddr = true;
                } else if (haveAddr) { // message codeword: 20 data bits
                    uint32_t d = (c >> 11) & 0xFFFFF;
                    for (int i = 19; i >= 0 && mbN < 120 * 8; i--) {
                        if (mbN % 8 == 0) msgBits[mbN / 8] = 0;
                        if ((d >> i) & 1) msgBits[mbN / 8] |= (0x80 >> (mbN % 8));
                        mbN++;
                    }
                }
            }
            if (haveAddr && mbN >= 12) {
                // try alphanumeric: 7-bit ASCII LSB-first
                int k = 0;
                for (int i = 0; i + 6 < mbN && k < 36; i += 7) {
                    char ch = 0;
                    for (int j = 0; j < 7; j++)
                        if (msgBits[(i + j) / 8] & (0x80 >> ((i + j) % 8))) ch |= (1 << j);
                    if (ch >= 0x20 && ch < 0x7F) text[k++] = ch;
                    else { text[k++] = ' '; }
                }
                text[k] = 0;
                if (k == 0) {
                    // numeric fallback: BCD nibbles (bit-reversed within
                    // each nibble), charset per POCSAG spec
                    static const char* num = "0123456789*U -)(";
                    for (int i = 0; i + 3 < mbN && k < 36; i += 4) {
                        uint8_t nib = 0;
                        for (int j = 0; j < 4; j++)
                            if (msgBits[(i + j) / 8] & (0x80 >> ((i + j) % 8))) nib |= (1 << j);
                        uint8_t rev = ((nib & 1) << 3) | ((nib & 2) << 1) | ((nib & 4) >> 1) | ((nib & 8) >> 3);
                        text[k++] = num[rev & 0xF];
                    }
                    text[k] = 0;
                }
                Msg& m = msgs[msgHead];
                msgHead = (msgHead + 1) % 4;
                m.addr = addr;
                strncpy(m.text, text, sizeof(m.text) - 1);
                m.text[sizeof(m.text) - 1] = 0;
                msgCount++;
            }
            pos = cwOff;
        }
    }

    // one sampling pass (~1.47 s), paced by micros()
    void samplePass() {
        rawN = 0;
        uint32_t nextUs = micros();
        const uint32_t bitUs = 833; // 1200 bps
        while (rawN < BITS) {
            while ((int32_t)(micros() - nextUs) < 0) {}
            nextUs += bitUs;
            if (rawN % 8 == 0) rawBits[rawN / 8] = 0;
            if (digitalRead(PIN_CC1101_GDO2)) rawBits[rawN / 8] |= (0x80 >> (rawN % 8));
            rawN++;
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "POCSAG RX");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "%.4f MHz  1200 bps  RX ONLY", FREQS[preset]);
        c.drawString(line, x0 + 10, y0 + 22);

        c.setTextColor(Theme::TEXT, Theme::BG);
        snprintf(line, sizeof(line), "syncs %lu   messages %lu",
                 (unsigned long)syncFound, (unsigned long)msgCount);
        c.drawString(line, x0 + 10, y0 + 40);

        for (int i = 0; i < 4; i++) {
            const Msg& m = msgs[(msgHead + i) % 4];
            int y = y0 + 62 + i * 24;
            if (!m.text[0]) continue;
            c.setTextColor(Theme::ACCENT, Theme::BG);
            snprintf(line, sizeof(line), "%07lu", (unsigned long)m.addr);
            c.drawString(line, x0 + 10, y);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(m.text, x0 + 78, y);
        }
        if (!monitoring) {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("idle", x0 + 10, y0 + 62);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(monitoring ? "listening..." : "rot: freq  click: start", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            preset = ((preset + in.rotationSteps) % 4 + 4) % 4;
            if (SubGhz::isReady()) setupFsk();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (!SubGhz::isReady()) return true;
            if (monitoring) {
                monitoring = false;
                restoreDefaults();
            } else {
                setupFsk();
                pinMode(PIN_CC1101_GDO2, INPUT);
                monitoring = true;
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            File f = SD.open("/pocsag.log", FILE_APPEND);
            if (f) {
                for (int i = 0; i < 4; i++) {
                    const Msg& m = msgs[(msgHead + i) % 4];
                    if (m.text[0]) f.printf("%lu,%s\n", (unsigned long)m.addr, m.text);
                }
                f.close();
            }
            snprintf(statusMsg, sizeof(statusMsg), "saved /pocsag.log");
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!monitoring) return;
        samplePass();
        decode();
        Features::requestRedraw();
    }

    void onExit() { if (monitoring) { monitoring = false; restoreDefaults(); } }
}

REGISTER_FEATURE("POCSAG RX", MwCat::SubGhz, MwIcon::Terminal, 40,
                 nullptr, input, tick, draw, onExit);
