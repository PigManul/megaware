// =====================================================================
//  Sub-GHz: CC1101 Console — manual radio lab ("Bus Pirate for RF").
//  Edit frequency / modulation / deviation / data rate / RX BW directly;
//  settings apply live. Hold: 1 s CW carrier (ISM bands only!).
//  On exit the driver is restored to the firmware defaults (OOK RX).
//  Rotation: select row / edit value. Click: enter edit / apply.
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>

namespace {
    enum Row { R_FREQ, R_MOD, R_DEV, R_RATE, R_BW, R_RSSI, ROWS };
    int row = 0;
    bool editing = false;

    float freq = 433.92f;
    int   mod = 0;        // 0 2-FSK, 1 GFSK, 2 ASK/OOK, 3 4-FSK
    float dev = 25.0f;    // kHz
    float drate = 9.6f;   // kbps
    float rxbw = 100.0f;  // kHz

    static const char* MOD_NAMES[4] = {"2-FSK", "GFSK", "ASK/OOK", "4-FSK"};
    static const float DEVS[4] = {25.0f, 50.0f, 100.0f, 150.0f};
    static const float RATES[6] = {1.2f, 2.4f, 9.6f, 19.2f, 38.4f, 100.0f};
    static const float BWS[5] = {58.0f, 100.0f, 135.0f, 200.0f, 270.0f};
    int devIdx = 0, rateIdx = 2, bwIdx = 1;

    void applyAll() {
        if (!SubGhz::isReady()) return;
        ELECHOUSE_cc1101.setCCMode(0); // async raw mode
        ELECHOUSE_cc1101.setModulation(mod);
        ELECHOUSE_cc1101.setDeviation(dev);
        ELECHOUSE_cc1101.setDRate(drate);
        ELECHOUSE_cc1101.setRxBW(rxbw);
        ELECHOUSE_cc1101.setPA(10);
        SubGhz::setFrequency(freq);
        ELECHOUSE_cc1101.SetRx();
    }

    void restoreDefaults() {
        if (!SubGhz::isReady()) return;
        ELECHOUSE_cc1101.setCCMode(0);
        ELECHOUSE_cc1101.setModulation(2); // OOK, firmware default
        ELECHOUSE_cc1101.setPA(10);
        SubGhz::setFrequency(SubGhz::currentFrequencyMHz());
        ELECHOUSE_cc1101.SetRx();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "CC1101 CONSOLE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        float rssi = SubGhz::isReady() ? SubGhz::readRssiDbm() : -128.0f;
        for (int i = 0; i < ROWS; i++) {
            const char* label;
            char val[24] = "";
            switch (i) {
                case R_FREQ: label = "Freq";    snprintf(val, sizeof(val), "%.2f MHz", freq); break;
                case R_MOD:  label = "Mod";     snprintf(val, sizeof(val), "%s", MOD_NAMES[mod]); break;
                case R_DEV:  label = "Dev";     snprintf(val, sizeof(val), "%.0f kHz", dev); break;
                case R_RATE: label = "Rate";    snprintf(val, sizeof(val), "%.1f kbps", drate); break;
                case R_BW:   label = "RX BW";   snprintf(val, sizeof(val), "%.0f kHz", rxbw); break;
                default:     label = "RSSI";
                    snprintf(val, sizeof(val), "%.0f dBm", rssi);
                    c.setTextColor(rssi > -80 ? Theme::OK : Theme::TEXT_FAINT, Theme::BG);
                    c.drawString("carrier present", x0 + 180, y0 + 24 + i * Theme::ROW_LIST);
                    break;
            }
            bool sel = (i == row);
            c.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(label, x0 + 10, y0 + 24 + i * Theme::ROW_LIST);
            c.setTextColor(sel && editing ? Theme::WARN : Theme::TEXT, Theme::BG);
            c.drawString(val, x0 + 90, y0 + 24 + i * Theme::ROW_LIST);
        }
        if (!SubGhz::isReady()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("CC1101 not found", x0 + 10, y0 + h - 30);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: select/edit   click: enter/apply", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (!SubGhz::isReady()) return in.event != InputEvent::None;
        if (in.rotationSteps != 0) {
            int st = in.rotationSteps;
            if (editing) {
                switch (row) {
                    case R_FREQ:  freq += st * 0.1f; if (freq < 300) freq = 300; if (freq > 930) freq = 930; break;
                    case R_MOD:   mod = ((mod + st) % 4 + 4) % 4; break;
                    case R_DEV:   devIdx = ((devIdx + st) % 4 + 4) % 4; dev = DEVS[devIdx]; break;
                    case R_RATE:  rateIdx = ((rateIdx + st) % 6 + 6) % 6; drate = RATES[rateIdx]; break;
                    case R_BW:    bwIdx = ((bwIdx + st) % 5 + 5) % 5; rxbw = BWS[bwIdx]; break;
                }
                applyAll();
            } else {
                row = ((row + st) % ROWS + ROWS) % ROWS;
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (row == R_RSSI) return true;
            editing = !editing;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            // 1 s unmodulated carrier - research/antenna test, ISM bands only
            ELECHOUSE_cc1101.SetTx();
            delay(1000);
            ELECHOUSE_cc1101.SetRx();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::UserKey) editing = false;
        return false;
    }

    void onExit() { restoreDefaults(); }
}

REGISTER_FEATURE("CC1101 Console", MwCat::SubGhz, MwIcon::Terminal, 25,
                 nullptr, input, nullptr, draw, onExit);
