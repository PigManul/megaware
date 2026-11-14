// =====================================================================
//  Tools: SUMP server — the board acts as an OpenBench Logic Sniffer
//  compatible logic analyzer over USB CDC, so SIGROK/PulseView on a PC
//  captures from this device.
//  Protocol subset (ols): ID (0x02 -> "1ALS002\0"), sample count/divisor
//  (0x80/0x81), run (0x01) -> uniform sampling of 4 GPIO channels into a
//  PSRAM buffer, streamed as big-endian 32-bit words.
//  Channels (bit -> GPIO): 0=17, 1=43, 2=44, 3=1.
//  Sample rate = 100 MHz / (divisor + 1), floored at 25 kHz by the ISR
//  period. Up to 65536 samples (128 KB PSRAM).
//  While SUMP is active the UI is parked: the USB serial belongs to the
//  client. Click or Quit returns to the firmware.
// =====================================================================
#include "../ui/features.h"
#include "../../include/theme.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>

namespace {
    bool active = false;
    uint32_t divisor = 1249;       // default ~80 kHz
    uint32_t limit = 4096;         // samples per run

    // capture channels
    static const int CH_PIN[4] = {17, 43, 44, 1};

    constexpr uint32_t SUMP_MAX = 65536;
    uint16_t* sumpBuf = nullptr;   // PSRAM, one uint16 per sample (4 ch)

    volatile uint32_t sumpPos = 0;
    volatile bool sumpRun = false;
    esp_timer_handle_t sTimer = nullptr;

    void IRAM_ATTR sumpIsr(void*) {
        if (!sumpRun || sumpPos >= limit || sumpBuf == nullptr) return;
        uint16_t v = 0;
        for (int b = 0; b < 4; b++)
            if (gpio_get_level((gpio_num_t)CH_PIN[b])) v |= (1 << b);
        sumpBuf[sumpPos++] = v;
    }

    void sumpStartCapture() {
        sumpPos = 0;
        sumpRun = true;
        uint64_t period = divisor + 1;             // *10 us base
        if (period < 40) period = 40;              // 25 kHz floor (40 us)
        esp_timer_stop(sTimer);
        esp_timer_start_periodic(sTimer, period * 10);
    }

    void sumpStopCapture() {
        sumpRun = false;
        esp_timer_stop(sTimer);
    }

    // configure input pins once
    void sumpInitPins() {
        for (int b = 0; b < 4; b++) {
            gpio_config_t cfg = {};
            cfg.pin_bit_mask = 1ULL << CH_PIN[b];
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&cfg);
        }
    }

    void run() {
        sumpStartCapture();
        while (sumpRun && sumpPos < limit) delay(1); // capture block (client expects the dump right away)
        sumpStopCapture();
        // stream: 32-bit big-endian per sample, low 16 bits carry the channels
        uint32_t n = sumpPos < limit ? sumpPos : limit;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w = sumpBuf[i];
            uint8_t b[4] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
            Serial.write(b, 4);
        }
        Serial.flush();
        Features::requestRedraw();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "SUMP SERVER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("sigrok / PulseView over USB CDC", x0 + 10, y0 + 22);
        snprintf(line, sizeof(line), "ch0 17  ch1 43  ch2 44  ch3 1");
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 36);
        snprintf(line, sizeof(line), "limit %lu  rate ~%lu kHz", (unsigned long)limit,
                 (unsigned long)(100000UL / (divisor + 1)));
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 50);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("PulseView: ols driver, serial port", x0 + 10, y0 + 64);
        c.setTextColor(active ? Theme::OK : Theme::ACCENT, Theme::BG);
        c.drawString(active ? "SUMP ACTIVE (UI parked)" : "click: activate", x0 + 10, y0 + h - 28);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click/quit: back to firmware", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            active = !active;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!active) return;
        // SUMP protocol loop over Serial (single command per tick keeps
        // the UI alive between client commands)
        while (Serial.available()) {
            uint8_t cmd = Serial.read();
            switch (cmd) {
                case 0x00: // reset
                    sumpStopCapture();
                    break;
                case 0x01: // RUN
                    run();
                    break;
                case 0x02: { // ID
                    Serial.write((const uint8_t*)"1ALS002", 8);
                    Serial.flush();
                    break;
                }
                case 0x80: { // sample count (4 bytes follow)
                    uint32_t v = 0;
                    uint32_t t0 = millis();
                    while (Serial.available() < 4 && millis() - t0 < 2000) delay(1);
                    for (int i = 0; i < 4; i++) {
                        while (!Serial.available()) delay(1);
                        v = (v << 8) | Serial.read();
                    }
                    limit = v + 1;
                    if (limit > SUMP_MAX) limit = SUMP_MAX;
                    if (limit < 4) limit = 4;
                    break;
                }
                case 0x81: { // divisor (4 bytes follow)
                    uint32_t v = 0;
                    uint32_t t0 = millis();
                    while (Serial.available() < 4 && millis() - t0 < 2000) delay(1);
                    for (int i = 0; i < 4; i++) {
                        while (!Serial.available()) delay(1);
                        v = (v << 8) | Serial.read();
                    }
                    divisor = v;
                    break;
                }
                case 0x82: // flags
                case 0x83: // trigger config
                case 0x84: case 0x85: case 0x86: case 0x87:
                case 0x88: case 0x89: case 0x8A: case 0x8B: {
                    uint32_t t0 = millis();
                    while (Serial.available() < 4 && millis() - t0 < 2000) delay(1);
                    for (int i = 0; i < 4; i++) {
                        while (!Serial.available()) delay(1);
                        Serial.read();
                    }
                    break;
                }
                default: break;
            }
        }
    }

    void onEnter() {
        sumpBuf = (uint16_t*)heap_caps_malloc(SUMP_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        sumpInitPins();
        esp_timer_create_args_t cfg = {};
        cfg.callback = &sumpIsr;
        cfg.name = "sump";
        esp_timer_create(&cfg, &sTimer);
    }

    void onExit() {
        sumpStopCapture();
        if (sTimer) { esp_timer_delete(sTimer); sTimer = nullptr; }
        if (sumpBuf) { free(sumpBuf); sumpBuf = nullptr; }
        active = false;
    }
}

REGISTER_FEATURE("SUMP Server", "Tools", MwIcon::Screwdriver, 35,
                 onEnter, input, tick, draw, onExit);
