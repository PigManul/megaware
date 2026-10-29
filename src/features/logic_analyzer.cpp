// =====================================================================
//  Tools: Logic Analyzer — capture of logic transitions on GPIO.
//  Pins: 17 (free), 43, 44 (free with USB CDC).
//  Rotation — pin select, click — start/stop capture.
//  Capture via GPIO ISR: a timestamp for each edge (±1 us).
//  Display: waveform on the TFT + edge counter.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <esp_timer.h>
#include <driver/gpio.h>

namespace {
    constexpr int LA_MAX = 512;
    static volatile int laCount = 0;
    static volatile uint32_t laTimes[LA_MAX];   // us from start
    static volatile uint8_t laLevels[LA_MAX];   // 0/1
    static bool capturing = false;
    static int laPin = 17;
    static int64_t laStartTime = 0;
    static const int LA_PINS[] = {17, 43, 44};
    static const char* LA_PIN_NAMES[] = {"GPIO 17", "GPIO 43", "GPIO 44"};
    static const int LA_PIN_N = 3;
    static int pinSel = 0;

    void IRAM_ATTR laIsr(void* arg) {
        if (laCount >= LA_MAX) return;
        int64_t now = esp_timer_get_time();
        if (laCount == 0) laStartTime = now;
        laTimes[laCount] = (uint32_t)(now - laStartTime);
        laLevels[laCount] = gpio_get_level((gpio_num_t)LA_PINS[pinSel]);
        laCount++;
    }

    void startCapture() {
        laCount = 0;
        int pin = LA_PINS[pinSel];
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&cfg);
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)pin, laIsr, nullptr);
        laStartTime = esp_timer_get_time();
        capturing = true;
    }

    void stopCapture() {
        gpio_isr_handler_remove((gpio_num_t)LA_PINS[pinSel]);
        capturing = false;
    }

    void drawWaveform(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        if (laCount < 2) return;
        uint32_t maxT = laTimes[laCount - 1];
        if (maxT == 0) return;
        float scale = (float)w / maxT;
        int midY = y0 + h / 2;
        int highY = midY - h / 4;
        int lowY = midY + h / 4;

        // draw waveform
        for (int i = 1; i < laCount; i++) {
            uint32_t t0 = laTimes[i - 1], t1 = laTimes[i];
            int x0p = x0 + (int)(t0 * scale);
            int x1 = x0 + (int)(t1 * scale);
            if (x1 > x0 + w) x1 = x0 + w;
            int level = laLevels[i - 1];
            int y = level ? highY : lowY;
            // vertical transition
            if (i == 1) { c.drawLine(x0p, highY, x0p, lowY, Theme::ACCENT); }
            // horizontal at level
            c.drawLine(x0p, y, x1, y, Theme::ACCENT);
            // vertical transition at t1
            c.drawLine(x1, highY, x1, lowY, Theme::ACCENT);
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "LOGIC ANALYZER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        // pin select
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("pin:", x0 + 10, y0 + 24);
        for (int i = 0; i < LA_PIN_N; i++) {
            int y = y0 + 24;
            int x = x0 + 34 + i * 62;
            bool sel = (pinSel == i);
            if (sel) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x - 12, y); }
            c.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(LA_PIN_NAMES[i], x, y);
        }

        if (capturing) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("CAPTURING...", x0 + 10, y0 + 46);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            char line[40];
            snprintf(line, sizeof(line), "edges: %d / %d", laCount, LA_MAX);
            c.drawString(line, x0 + 10, y0 + 64);
            if (laCount >= LA_MAX) { stopCapture(); }
            return;
        }

        if (laCount >= 2) {
            // waveform
            int waveY = y0 + 60;
            int waveH = h - 90;
            c.fillRect(x0 + 8, waveY, w - 16, waveH, Theme::BG);
            c.drawRect(x0 + 8, waveY, w - 16, waveH, Theme::BORDER);
            uint32_t maxT = laTimes[laCount - 1];
            if (maxT > 0) {
                float scale = (float)(w - 20) / maxT;
                int midY = waveY + waveH / 2;
                int highY = waveY + waveH / 4;
                int lowY = waveY + waveH * 3 / 4;
                c.setTextDatum(textdatum_t::middle_center);
                for (int i = 1; i < laCount && i < 200; i++) {
                    uint32_t t0 = laTimes[i-1], t1 = laTimes[i];
                    int xp = x0 + 8 + (int)(t0 * scale);
                    int xn = x0 + 8 + (int)(t1 * scale);
                    if (xn > x0 + w - 8) xn = x0 + w - 8;
                    if (xn <= xp) xn = xp + 1;
                    int y = laLevels[i-1] ? highY : lowY;
                    c.drawLine(xp, y, xn, y, Theme::ACCENT);
                    c.drawLine(xn, highY, xn, lowY, Theme::ACCENT);
                }
            }
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            char line[40];
            snprintf(line, sizeof(line), "edges: %d  span: %lu us", laCount,
                     (unsigned long)(laCount > 1 ? laTimes[laCount-1] : 0));
            c.drawString(line, x0 + 10, y0 + 44);
        }

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: pin  click: start/stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            if (capturing) return true;
            pinSel = (pinSel + in.rotationSteps) % LA_PIN_N;
            if (pinSel < 0) pinSel += LA_PIN_N;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (capturing) stopCapture();
            else startCapture();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (capturing && laCount >= LA_MAX) {
            stopCapture();
            Features::requestRedraw();
        }
        if (laCount > 2 && capturing) {
            static uint32_t lastUi = 0;
            if (millis() - lastUi > 300) { lastUi = millis(); Features::requestRedraw(); }
        }
    }

    void onExit() {
        if (capturing) stopCapture();
    }
}

REGISTER_FEATURE("Logic Analyzer", "Tools", MwIcon::Screwdriver, 30,
                 nullptr, input, tick, draw, onExit);
