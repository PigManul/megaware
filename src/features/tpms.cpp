// =====================================================================
//  Sub-GHz: TPMS — tire pressure sensor receiver / car detector.
//  Every car transmits 433.92 (EU/Asia) or 315 MHz (US) FSK bursts.
//  Receive: CC1101 2-FSK async on GDO2 -> edge capture -> Manchester
//  bit reconstruction (both polarities tried, preamble-scored) ->
//  hex frame view. The reliable part is the DETECTOR: every valid
//  burst = a car passed by. Decoder is experimental (protocols vary
//  by vendor): the hex dump is the research artifact, first bytes are
//  typically the sensor ID. Long press: save raw burst as .sub.
//  Rotation: 433.92 / 315.00 preset. Click: arm/cancel capture.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    bool armed = false;
    bool capturing = false;
    int preset = 0; // 0=433.92, 1=315.00
    uint32_t carsSeen = 0;
    uint32_t burstsTotal = 0;
    char lastHex[44] = "";
    int lastBits = 0;
    char statusMsg[48] = "";
    RawCapture cap;

    static const float PRESETS[2] = {433.92f, 315.00f};

    void setupFsk() {
        if (!SubGhz::isReady()) return;
        ELECHOUSE_cc1101.setCCMode(0);
        ELECHOUSE_cc1101.setModulation(0); // 2-FSK
        ELECHOUSE_cc1101.setDeviation(25.0f);
        ELECHOUSE_cc1101.setDRate(19.2f);
        ELECHOUSE_cc1101.setRxBW(100.0f);
        ELECHOUSE_cc1101.setPA(10);
        SubGhz::setFrequency(PRESETS[preset]);
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

    // Manchester decode with polarity scoring: fills bits[64], returns
    // the decoded window length (64 bytes fixed view)
    int decodeManchester(const RawCapture& c, uint8_t* bits, uint32_t halfUs) {
        static uint8_t cells[2048];
        int cellsN = 0;
        bool level = true;
        for (size_t i = 0; i < c.count && cellsN < 2048; i++) {
            uint32_t u = (c.durationsUs[i] + halfUs / 2) / halfUs;
            if (u < 1) u = 1;
            for (uint32_t k = 0; k < u && cellsN < 2048; k++) cells[cellsN++] = level;
            level = !level;
        }
        if (cellsN < 64) return 0;
        uint8_t cand[2][64];
        int score[2] = {0, 0};
        for (int inv = 0; inv < 2; inv++) {
            int n = 0;
            for (int i = 0; i + 1 < cellsN && n < 64 * 8; i += 2) {
                uint8_t lo = cells[i + 1] ^ inv;
                if (n % 8 == 0) cand[inv][n / 8] = 0;
                if (lo) cand[inv][n / 8] |= (0x80 >> (n % 8)); // 01 -> 1 (rising mid-bit)
                n++;
            }
            // score: alternating-bit preamble (0x55/0xAA family) near start
            for (int b = 0; b < 8; b++) {
                uint8_t x = cand[inv][b] ^ (cand[inv][b] >> 1);
                if (!(x & 0x80)) score[inv]++; // each alternating bit pair
            }
        }
        memcpy(bits, score[1] > score[0] ? cand[1] : cand[0], 64);
        return 64 * 8;
    }

    void finishCapture() {
        burstsTotal++;
        if (!cap.valid || cap.count < 20) { snprintf(statusMsg, sizeof(statusMsg), "no burst"); return; }
        // half-bit period for 19.2 kbps Manchester = 1/38400 s ~ 26 us;
        // use unit from the shortest pulse (real data) instead of theory
        uint32_t minUs = 0xFFFFFFFF;
        for (size_t i = 0; i < cap.count; i++)
            if (cap.durationsUs[i] < minUs && cap.durationsUs[i] >= 40) minUs = cap.durationsUs[i];
        if (minUs == 0xFFFFFFFF) { snprintf(statusMsg, sizeof(statusMsg), "no edges"); return; }
        uint32_t halfUs = minUs; // shortest level segment ~= half bit
        static uint8_t bits[64];
        decodeManchester(cap, bits, halfUs);
        lastBits = 64 * 8;
        for (int i = 0; i < 21; i++) snprintf(lastHex + i * 2, 3, "%02X", bits[i]);
        carsSeen++;
        snprintf(statusMsg, sizeof(statusMsg), "burst decoded (unit %lu us)", (unsigned long)halfUs);
        Features::requestRedraw();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TPMS / CAR DETECTOR");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "RX %.2f MHz  2-FSK 19.2k (exp)", PRESETS[preset]);
        c.drawString(line, x0 + 10, y0 + 22);

        c.setTextColor(Theme::ACCENT, Theme::BG);
        snprintf(line, sizeof(line), "cars detected: %lu", (unsigned long)carsSeen);
        c.drawString(line, x0 + 10, y0 + 44);

        c.setTextColor(Theme::TEXT, Theme::BG);
        snprintf(line, sizeof(line), "bursts %lu   last frame:", (unsigned long)burstsTotal);
        c.drawString(line, x0 + 10, y0 + 66);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(lastHex[0] ? lastHex : "----", x0 + 10, y0 + 82);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("hex = manchester view; sensor id is", x0 + 10, y0 + 100);
        c.drawString("usually the first bytes (vendor-specific)", x0 + 10, y0 + 110);

        if (statusMsg[0]) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 32);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(capturing ? "listening..." : "rot: band  click: arm  hold: save", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            preset = (preset + 1) % 2;
            if (SubGhz::isReady()) setupFsk();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (!SubGhz::isReady() || capturing) return true;
            setupFsk();
            SubGhz::startRawCapture(&cap, 4000);
            capturing = true;
            snprintf(statusMsg, sizeof(statusMsg), "armed (4 s window)");
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress && cap.valid) {
            if (!SD.exists("/subghz") && !LittleFS.exists("/subghz")) SD.mkdir("/subghz");
            String path = "/subghz/tpms_" + String(millis()) + ".sub";
            bool ok = SubGhz::saveRaw(path, cap);
            snprintf(statusMsg, sizeof(statusMsg), ok ? "saved %s" : "save failed", ok ? path.c_str() : "");
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (capturing && !SubGhz::isCapturing()) {
            SubGhz::stopRawCapture();
            capturing = false;
            finishCapture();
            Features::requestRedraw();
        }
    }

    void onExit() { restoreDefaults(); }
}

REGISTER_FEATURE("TPMS", MwCat::SubGhz, MwIcon::Pulse, 35,
                 nullptr, input, tick, draw, onExit);
