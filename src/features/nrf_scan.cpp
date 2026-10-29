// =====================================================================
//  RF24: Scanner — 2.4 GHz activity across channels 0..126. An external
//  NRF24 module in the Grove port is REQUIRED (CE=43, CS=44, SPI 9/10/11). The bus
//  runs on a separate controller (HSPI) so it does not conflict with the display.
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

    uint8_t levels[126] = {0};    // per-channel activity (0..~5)
    uint32_t lastSweep = 0;

    bool initRadio() {
        if (radioOk) return true;
        if (!inited) {
            // do NOT pass ss: the NRF24 CSN is toggled via digitalWrite,
            // routing the SS signal to pin 44 would break that
            SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
            inited = true;
        }
        radioOk = radio.begin(&SPI) && radio.isChipConnected();
        if (radioOk) radio.stopListening();
        return radioOk;
    }

    void sweep() {
        // 127 channels at ~300 us each = ~40 ms per pass
        radio.stopListening();
        for (int ch = 0; ch < 126; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(280);
            bool active = radio.testCarrier() || radio.testRPD();
            radio.stopListening();
            if (active) {
                if (levels[ch] < 250) levels[ch] += 8;   // fast rise
            } else if (levels[ch]) {
                levels[ch] -= 1;                          // slow decay
            }
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NRF24 SCANNER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!initRadio()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("NRF24 not detected", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Grove: CE 43, CS 44, SPI 9/10/11", x0 + 10, y0 + 42);
            return;
        }

        int baseY = y0 + h - 18;
        int bw = (w - 8) / 126;
        if (bw < 2) bw = 2;
        c.setTextDatum(textdatum_t::middle_center);
        for (int ch = 0; ch < 126; ch++) {
            int bh = levels[ch] * (h - 46) / 250;
            if (bh > 0) c.fillRect(x0 + 4 + ch * bw, baseY - bh, bw - 1, bh, Theme::ACCENT);
        }
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("2400", x0 + 10, baseY + 8);
        c.drawString("2525 MHz", x0 + w - 30, baseY + 8);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("external module (Grove port)", x0 + 10, y0 + 22);
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        if (!radioOk && !initRadio()) return;
        if (millis() - lastSweep >= 80) {  // ~10 passes/s
            lastSweep = millis();
            sweep();
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (radioOk) radio.powerDown();
    }
}

REGISTER_FEATURE("NRF Scan", "RF24", MwIcon::Nrf, 10,
                 nullptr, input, tick, draw, onExit);
