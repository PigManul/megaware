// =====================================================================
//  RF24: Scan — LIVE oscilloscope of the 2.4 GHz band.
//  Fast, responsive view: short decay, per-channel carrier probe while
//  staying in RX (no per-channel mode switching - the PLL relocks in
//  ~150 us, so a full 126-channel pass takes ~30-120 ms depending on
//  the dwell setting). Rotation: dwell speed. Click: pause/resume.
//  A twin feature, "Wideband", is the slow accumulating ANALYZER with
//  traffic classification - this one is the fast eyeball.
//  External NRF24 module in the Grove port (CE 43, CS 44, SPI 9/10/11).
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <SPI.h>
#include <RF24.h>

namespace {
    // IMPORTANT (bug "UI freezes after entering NRF24"): this used to
    // create a SEPARATE SPIClass(HSPI) controller and begin(11,10,9,44)
    // re-routed GPIO 9/10/11 to SPI3 — the display (SPI2) lost those
    // output signals and the screen froze. Now the same
    // SPI2 controller as the display is used: RF24 operations are short and run
    // between frames, transactions are atomic.
    RF24 radio(PIN_NRF_CE, PIN_NRF_CS, 1000000);
    bool radioOk = false;
    bool inited = false;

    constexpr int MAX_LVL = 100;
    uint8_t levels[126] = {0};
    uint32_t lastSweep = 0;
    uint32_t sweeps = 0;
    bool paused = false;

    // dwell presets (us): capped so one blocking pass stays under ~80 ms
    static const uint16_t DWELLS[4] = {150, 280, 450, 600};
    int dwellIdx = 1;

    bool initRadio() {
        if (radioOk) return true;
        if (!inited) {
            // do NOT pass ss: the NRF24 CSN is toggled via digitalWrite,
            // routing the SS signal to pin 44 would break that
            SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
            inited = true;
        }
        radioOk = radio.begin(&SPI) && radio.isChipConnected();
        if (radioOk) {
            radio.stopListening();
            radio.startListening(); // stay in RX: only setChannel() per step
        }
        return radioOk;
    }

    void sweep() {
        // stay in RX mode, relock PLL per channel - no CE toggling
        for (int ch = 0; ch < 126; ch++) {
            radio.setChannel(ch);
            delayMicroseconds(DWELLS[dwellIdx]);
            bool active = radio.testCarrier() || radio.testRPD();
            if (active) {
                if (levels[ch] < MAX_LVL) levels[ch] += 12;  // fast rise
            } else if (levels[ch]) {
                levels[ch] -= 5;                             // fast decay: live picture
            }
        }
        sweeps++;
    }

    int peakCh() {
        int peak = 0;
        for (int ch = 0; ch < 126; ch++) if (levels[ch] > levels[peak]) peak = ch;
        return peak;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "RF SCAN (LIVE)");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        if (!initRadio()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("NRF24 not detected", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Grove: CE 43, CS 44, SPI 9/10/11", x0 + 10, y0 + 42);
            return;
        }

        c.setTextColor(paused ? Theme::WARN : Theme::OK, Theme::BG);
        snprintf(line, sizeof(line), "%s  sweep %lu  dwell %u us",
                 paused ? "PAUSED" : "running", (unsigned long)sweeps, DWELLS[dwellIdx]);
        c.drawString(line, x0 + 10, y0 + 20);

        int p = peakCh();
        c.setTextColor(Theme::ACCENT, Theme::BG);
        snprintf(line, sizeof(line), "peak ch %d (%.3f GHz)  lvl %d%%",
                 p, 2.400f + p * 0.001f, levels[p] * 100 / MAX_LVL);
        c.drawString(line, x0 + 10, y0 + 32);

        int baseY = y0 + h - 18;
        int bw = (w - 8) / 126;
        if (bw < 2) bw = 2;
        for (int ch = 0; ch < 126; ch++) {
            int bh = levels[ch] * (h - 60) / MAX_LVL;
            if (bh > 0) c.fillRect(x0 + 4 + ch * bw, baseY - bh, bw - 1, bh, Theme::ACCENT);
        }
        c.drawFastHLine(x0 + 4, baseY, w - 8, Theme::TEXT_DIM);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("2400", x0 + 6, baseY + 4);
        c.drawString("2462", x0 + w / 2 - 14, baseY + 4);
        c.drawString("2525 MHz", x0 + w - 42, baseY + 4);

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: dwell speed   click: pause/resume", x0 + 10, y0 + h - 30);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            dwellIdx = (dwellIdx + in.rotationSteps + 4) % 4;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            paused = !paused;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            memset(levels, 0, sizeof(levels)); // clear the picture
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!radioOk && !initRadio()) return;
        if (paused) return;
        if (millis() - lastSweep >= 60) {
            lastSweep = millis();
            sweep();
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (radioOk) {
            radio.stopListening();
            radio.powerDown();
        }
    }
}

REGISTER_FEATURE("NRF Scan", "RF24", MwIcon::Nrf, 10,
                 nullptr, input, tick, draw, onExit);
