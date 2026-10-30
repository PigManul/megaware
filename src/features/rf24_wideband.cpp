// =====================================================================
//  RF24: Wideband Monitor — full-band 2.4 GHz occupancy analyzer.
//  Sweeps all 126 nRF24 channels (2.400-2.525 GHz) reading the RPD/CD
//  carrier detect register per channel, then classifies the traffic by
//  pattern:
//    - Wi-Fi: a burning block of ~20 consecutive channels (20 MHz)
//    - BLE: narrow peaks on the advertisement channels 2402/2426/2480
//    - static: one channel continuously hot (fixed-carrier device)
//    - hopper: energy jumping across many channels (2.4 GHz mice etc.)
//  Also reports the cleanest channel - where to put your own RF24 /
//  ESP-NOW link so Wi-Fi does not step on it.
//  External NRF24 module in the Grove port (CE 43, CS 44, SPI 9/10/11).
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <SPI.h>
#include <RF24.h>

namespace {
    RF24 radio(PIN_NRF_CE, PIN_NRF_CS, 1000000);
    bool radioOk = false;
    bool inited = false;

    uint8_t occ[126] = {0};       // per-channel occupancy 0..255
    uint32_t sweeps = 0;
    uint32_t lastSweep = 0;

    // classification state (recomputed per sweep)
    int wifiBlocks = 0;           // count of detected ~20 MHz burning blocks
    bool bleSeen = false;
    bool staticCarrier = false;
    bool hopper = false;
    int busiestCh = 0;
    int cleanCh = 40;

    bool initRadio() {
        if (radioOk) return true;
        if (!inited) {
            SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
            inited = true;
        }
        radioOk = radio.begin(&SPI) && radio.isChipConnected();
        if (radioOk) radio.stopListening();
        return radioOk;
    }

    void classify() {
        wifiBlocks = 0;
        bleSeen = false;
        staticCarrier = false;
        hopper = false;
        int hot = 0, warm = 0;
        busiestCh = 0;
        for (int ch = 0; ch < 126; ch++) {
            if (occ[ch] > occ[busiestCh]) busiestCh = ch;
            if (occ[ch] > 60) hot++;
            else if (occ[ch] > 20) warm++;
        }
        // Wi-Fi blocks: runs of >=14 hot channels
        int run = 0;
        for (int ch = 0; ch < 126; ch++) {
            if (occ[ch] > 60) {
                run++;
                if (run == 14) wifiBlocks++;
            } else run = 0;
        }
        // BLE: adv channels 2 / 26 / 50 above their neighborhood
        static const int ADV[3] = {2, 26, 50};
        bleSeen = true;
        for (int a = 0; a < 3; a++) {
            int ch = ADV[a];
            if (occ[ch] <= occ[ch - 3] + 6 && occ[ch] <= occ[ch + 3] + 6) bleSeen = false;
        }
        // static: one hot channel with cold neighbors
        for (int ch = 1; ch < 125; ch++) {
            if (occ[ch] > 180 && occ[ch - 1] < 30 && occ[ch + 1] < 30) staticCarrier = true;
        }
        // hopper: warm energy scattered over many channels
        if (warm > 25 && hot < 12) hopper = true;

        // cleanest: lowest occupancy away from detected Wi-Fi blocks
        cleanCh = 40;
        int best = 255;
        int run2 = 0;
        for (int ch = 10; ch < 116; ch++) {
            if (occ[ch] > 60) run2++;
            else run2 = 0;
            if (run2 > 0 && occ[ch] > 60) continue; // inside a block
            if (occ[ch] < best) { best = occ[ch]; cleanCh = ch; }
        }
    }

    void sweep() {
        radio.stopListening();
        for (int ch = 0; ch < 126; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(300);
            bool active = radio.testCarrier() || radio.testRPD();
            radio.stopListening();
            if (active) {
                if (occ[ch] < 235) occ[ch] += 20;  // fast rise
            } else if (occ[ch]) {
                occ[ch] -= 1;                      // slow decay
            }
        }
        sweeps++;
        classify();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "RF24 WIDEBAND");
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

        // classification
        c.setTextColor(Theme::TEXT, Theme::BG);
        snprintf(line, sizeof(line), "sweeps %lu  busy:", (unsigned long)sweeps);
        c.drawString(line, x0 + 10, y0 + 20);
        int tx = x0 + 78;
        if (wifiBlocks) {
            c.setTextColor(Theme::WARN, Theme::BG);
            snprintf(line, sizeof(line), "WIFI x%d", wifiBlocks);
            c.drawString(line, tx, y0 + 20);
            tx += 52;
        }
        if (bleSeen) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("BLE", tx, y0 + 20);
            tx += 30;
        }
        if (staticCarrier) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("STATIC", tx, y0 + 20);
            tx += 46;
        }
        if (hopper) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("HOPPER", tx, y0 + 20);
        }

        // channel map: 126 bars
        int mapY = y0 + 40;
        int mapH = h - 74;
        int bw = (w - 8) / 126;
        if (bw < 2) bw = 2;
        for (int ch = 0; ch < 126; ch++) {
            int bh = occ[ch] * mapH / 255;
            if (bh <= 0) continue;
            bool wifiCol = occ[ch] > 60;
            c.fillRect(x0 + 4 + ch * bw, mapY + mapH - bh, bw - 1, bh,
                       wifiCol ? Theme::WARN : Theme::ACCENT);
        }
        // baseline + scale
        c.drawFastHLine(x0 + 4, mapY + mapH, w - 8, Theme::TEXT_DIM);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("2400", x0 + 6, mapY + mapH + 4);
        c.drawString("2462", x0 + w / 2 - 14, mapY + mapH + 4);
        c.drawString("2525", x0 + w - 30, mapY + mapH + 4);

        c.setTextColor(Theme::OK, Theme::BG);
        snprintf(line, sizeof(line), "cleanest: ch %d (%.3f GHz)  peak ch %d (%.3f)",
                 cleanCh, 2.400f + cleanCh * 0.001f, busiestCh, 2.400f + busiestCh * 0.001f);
        c.drawString(line, x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        if (!radioOk && !initRadio()) return;
        if (millis() - lastSweep >= 70) {
            lastSweep = millis();
            sweep();
            if (sweeps % 3 == 0) Features::requestRedraw();
        }
    }

    void onExit() {
        if (radioOk) radio.powerDown();
    }
}

REGISTER_FEATURE("Wideband", "RF24", MwIcon::Nrf, 20,
                 nullptr, input, tick, draw, onExit);
