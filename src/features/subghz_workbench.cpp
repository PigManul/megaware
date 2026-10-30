// =====================================================================
//  Sub-GHz: RF Workbench — on-device reverse engineering of unknown
//  signals. Capture a burst (raw edges from CC1101 GDO2), then:
//    - pulse-width histogram (top buckets) -> timing unit estimate
//    - bit reconstruction from the unit -> hex view of the frame
//    - save the raw burst as .sub (Flipper RAW) for further analysis
//  Click: capture. Long press: save raw to /subghz/wb_<ms>.sub.
//  Research tool: analysis of captured air only.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State { Idle, Capturing, Analyzed };
    State state = Idle;

    RawCapture cap;
    uint32_t unitUs = 0;      // estimated shortest meaningful pulse
    uint32_t baudEst = 0;     // 1e6 / (2*unit) for Manchester-ish OOK
    int totalBits = 0;
    uint8_t bits[96];         // decoded bits (bit-packed), up to 768
    char hexStr[130] = "";
    int hist[6] = {0};        // bucket counts: ~unit, 2u, 3u..4u, 5u..8u, 9u..16u, >16u
    char statusMsg[48] = "";

    void analyze() {
        statusMsg[0] = 0;
        totalBits = 0;
        hexStr[0] = 0;
        for (int i = 0; i < 6; i++) hist[i] = 0;
        if (!cap.valid || cap.count < 8) {
            snprintf(statusMsg, sizeof(statusMsg), "too few edges (%lu)", (unsigned long)cap.count);
            return;
        }
        // histogram of pulse widths, log-ish buckets relative to the
        // shortest pulse (the "unit")
        uint32_t minUs = 0xFFFFFFFF;
        for (size_t i = 0; i < cap.count; i++)
            if (cap.durationsUs[i] < minUs && cap.durationsUs[i] >= 40) minUs = cap.durationsUs[i];
        if (minUs == 0xFFFFFFFF) { snprintf(statusMsg, sizeof(statusMsg), "no usable pulses"); return; }
        unitUs = minUs;

        for (size_t i = 0; i < cap.count; i++) {
            uint32_t d = cap.durationsUs[i];
            uint32_t u = (d + unitUs / 2) / unitUs; // in units
            if (u <= 1) hist[0]++;
            else if (u == 2) hist[1]++;
            else if (u <= 4) hist[2]++;
            else if (u <= 8) hist[3]++;
            else if (u <= 16) hist[4]++;
            else hist[5]++;
        }
        // PAM-ish OOK remotes: unit ~ one bit cell -> data rate ~ 1e6/unit
        baudEst = unitUs ? (1000000UL / unitUs) : 0;

        // bit reconstruction: walk the edge train, each unit of current
        // level contributes one bit cell; a full cell of constant level
        // = repeated bit. Pulse-position families (Princeton/CAME) map
        // naturally: short-high+long-low / long-high+short-low = 0/1.
        static uint8_t cells[2048];
        int cellCount = 0;
        bool level = true; // GDO2 idle low, first edge raises
        for (size_t i = 0; i < cap.count && cellCount < 2048; i++) {
            uint32_t u = (cap.durationsUs[i] + unitUs / 2) / unitUs;
            if (u < 1) u = 1;
            for (uint32_t k = 0; k < u && cellCount < 2048; k++) cells[cellCount++] = level ? 1 : 0;
            level = !level;
        }
        // fold cell pairs into bits by majority rules used by PPM remotes:
        // (1,0) -> 0, (0,1) -> 1; pairs (1,1)/(0,0) pass through as-is
        for (int i = 0; i + 1 < cellCount && totalBits < 96 * 8; i += 2) {
            uint8_t b;
            if (cells[i] == 1 && cells[i + 1] == 0) b = 0;
            else if (cells[i] == 0 && cells[i + 1] == 1) b = 1;
            else b = cells[i]; // ambiguous pair, keep raw
            bits[totalBits / 8] = (totalBits % 8 == 0) ? 0 : bits[totalBits / 8];
            if (b) bits[totalBits / 8] |= (0x80 >> (totalBits % 8));
            totalBits++;
        }
        if (totalBits <= 0) { snprintf(statusMsg, sizeof(statusMsg), "decode failed"); return; }
        int hexLen = (totalBits + 7) / 8;
        if (hexLen > 64) hexLen = 64;
        for (int i = 0; i < hexLen; i++)
            snprintf(hexStr + i * 2, 3, "%02X", bits[i]);
        snprintf(hexStr + hexLen * 2, 3, totalBits > hexLen * 8 ? ".." : "");
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
            c.drawString("click: capture burst", x0 + 10, y0 + 44);
        } else if (state == Capturing) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("capturing (GDO2 edges)...", x0 + 10, y0 + 44);
        } else {
            snprintf(line, sizeof(line), "edges %lu  unit %lu us  ~%lu us/cell",
                     (unsigned long)cap.count, (unsigned long)unitUs, (unsigned long)baudEst);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 40);

            // histogram bars (6 buckets)
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("pulse histogram (1u 2u 3-4u 5-8u 9-16u >16u):", x0 + 10, y0 + 56);
            int maxH = 1;
            for (int i = 0; i < 6; i++) if (hist[i] > maxH) maxH = hist[i];
            for (int i = 0; i < 6; i++) {
                int bh = hist[i] * 26 / maxH;
                c.fillRect(x0 + 10 + i * 22, y0 + 88 - bh, 16, bh, Theme::ACCENT);
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
                snprintf(line, sizeof(line), "%lu", (unsigned long)hist[i]);
                c.drawString(line, x0 + 10 + i * 22, y0 + 90);
            }

            snprintf(line, sizeof(line), "bits %d", totalBits);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 104);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(hexStr, x0 + 10, y0 + 120);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("(PPM fold: 10->0 01->1, see notes)", x0 + 10, y0 + 136);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 44);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: capture   hold: save .sub", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
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
