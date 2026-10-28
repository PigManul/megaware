// =====================================================================
//  Sub-GHz: Capture — RAW-захват, реплей и сохранение в формате
//  Flipper (.sub). Управление:
//    клик:  начать захват / остановить захват / реплей
//    hold:  сохранить захват (SD, иначе Flash) в /subghz/cap_<ms>.sub
//  Частота — текущая полоса (SubGhz::currentFrequencyMHz).
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { Idle, Capturing, Captured };
    State state = Idle;
    uint32_t captureStart = 0;   // старт захвата или момент реплея
    uint32_t savedShowUntil = 0; // сколько ещё показывать «SAVED <path>»
    char savedPath[64] = "";
    uint32_t captureMs = 3000; // длина захвата, вращением 1..10 с

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "RAW CAPTURE");
        RawCapture& cap = SubGhz::sharedCapture();

        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Capturing) {
            uint32_t elapsed = millis() - captureStart;
            c.setTextColor(Theme::ACCENT, Theme::BG_PANEL);
            c.drawString("LISTENING... (click: stop)", x0 + 10, y0 + 26);
            char line[40];
            snprintf(line, sizeof(line), "Edges  %d / %d", (int)cap.count, (int)RawCapture::MAX_EDGES);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
            c.drawString(line, x0 + 10, y0 + 44);
            float pct = (float)elapsed / (float)captureMs;
            c.fillRect(x0 + 10, y0 + h - 14, (int)((w - 20) * constrain(pct, 0.0f, 1.0f)), 4, Theme::ACCENT_DIM);
        } else if (state == Captured) {
            bool tx = millis() - captureStart < 800;
            c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
            c.drawString(tx ? "REPLAYING..." : "SIGNAL CAPTURED", x0 + 10, y0 + 26);
            char line[48];
            snprintf(line, sizeof(line), "Edges  %d", (int)cap.count);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
            c.drawString(line, x0 + 10, y0 + 44);
            snprintf(line, sizeof(line), "Freq   %.2f MHz", SubGhz::currentFrequencyMHz());
            c.drawString(line, x0 + 10, y0 + 58);
            if (savedShowUntil && millis() < savedShowUntil) {
                c.setTextColor(Theme::OK, Theme::BG_PANEL);
                c.drawString(savedPath, x0 + 10, y0 + 76);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("click: replay   hold: save .sub", x0 + 10, y0 + h - 14);
        } else {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
            c.drawString("Ready to listen on", x0 + 10, y0 + 26);
            char line[40];
            snprintf(line, sizeof(line), "%.2f MHz", SubGhz::currentFrequencyMHz());
            c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
            c.drawString(line, x0 + 10, y0 + 42);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("rotate: len  click: capture", x0 + 10, y0 + h - 14);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (state == Idle) {
                RawCapture& cap = SubGhz::sharedCapture();
                cap.count = 0;
                cap.valid = false;
                SubGhz::startRawCapture(&cap, captureMs);
                captureStart = millis();
                state = Capturing;
            } else if (state == Capturing) {
                SubGhz::stopRawCapture();
                state = SubGhz::sharedCapture().valid ? Captured : Idle;
            } else {
                captureStart = millis();
                SubGhz::replayRaw(SubGhz::sharedCapture());
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress && state == Captured) {
            RawCapture& cap = SubGhz::sharedCapture();
            if (cap.valid) {
                if (Files::sdPresent()) SD.mkdir("/subghz");
                else LittleFS.mkdir("/subghz");
                snprintf(savedPath, sizeof(savedPath), "/subghz/cap_%lu.sub", (unsigned long)millis());
                if (SubGhz::saveRaw(savedPath, cap)) savedShowUntil = millis() + 2500;
                else savedShowUntil = 0;
            }
            Features::requestRedraw();
            return true;
        }
        if (in.rotationSteps != 0 && state == Capturing) {
            Features::requestRedraw(); // живой счётчик фронтов
            return true;
        }
        return false;
    }

    void tick() {
        if (state == Capturing) {
            if (!SubGhz::isCapturing()) {
                state = SubGhz::sharedCapture().valid ? Captured : Idle;
                Features::requestRedraw();
            } else {
                static uint32_t last = 0;
                if (millis() - last > 100) { last = millis(); Features::requestRedraw(); }
            }
        }
    }
}

REGISTER_FEATURE("Capture", MwCat::SubGhz, MwIcon::None, 2,
                 nullptr, input, tick, draw, nullptr);
