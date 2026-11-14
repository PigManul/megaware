// =====================================================================
//  Tools: Continuity tester — probes on GPIO1 (3.3 V drive) and GPIO2
//  (ADC1 sense, internal pulldown). Voltage across the sense pin gives
//  a rough resistance estimate:
//      V = 3.3 * Rpd / (Rext + Rpd)   ->   Rext ~ Rpd * (3.3 - V) / V
//  Rpd (internal pulldown) varies 40..80 kOhm, so the reading is a
//  rough band, not a multimeter. Speaker beeps below 2 kOhm.
//  NOTE: pins 1/2 are the IR TX/RX - IR features pause during the test.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"

namespace {
    bool running = false;
    float lastV = 0;
    float lastOhm = 0;
    bool lastCont = false;

    // speaker beep via LEDC on the I2S data pin (I2S not installed here)
    void beep(int ms) {
        ledcSetup(0, 4000, 8);
        ledcAttachPin(PIN_SPK_DOUT, 0);
        ledcWriteTone(0, 4000);
        delay(ms);
        ledcWriteTone(0, 0);
        ledcDetachPin(PIN_SPK_DOUT);
    }

    void measure() {
        pinMode(PIN_IR_TX, OUTPUT);
        digitalWrite(PIN_IR_TX, HIGH);          // 3.3 V into the probe
        pinMode(PIN_IR_RX, INPUT_PULLDOWN);
        delay(2);
        uint32_t acc = 0;
        for (int i = 0; i < 16; i++) acc += analogRead(PIN_IR_RX);
        lastV = (acc / 16.0f) * 3.3f / 4095.0f;
        pinMode(PIN_IR_RX, INPUT);              // release the pulldown
        if (lastV <= 0.01f) {
            lastOhm = 0;
            lastCont = false;
        } else {
            lastOhm = 55000.0f * (3.3f - lastV) / lastV; // Rpd ~55 kOhm typical
            lastCont = lastV > 2.2f;                 // roughly < 2 kOhm
        }
        if (lastCont) beep(60);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "CONTINUITY");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("probes: GPIO1 (+3.3V) / GPIO2 (ADC)", x0 + 10, y0 + 22);
        c.drawString("IR paused while measuring", x0 + 10, y0 + 34);

        if (running) {
            if (lastCont) {
                c.setTextColor(Theme::OK, Theme::BG);
                c.setTextSize(Theme::SIZE_MENU);
                c.drawString("CONTINUITY", x0 + 10, y0 + 56);
                c.setTextSize(Theme::SIZE_TEXT);
            } else if (lastOhm > 0) {
                c.setTextColor(Theme::WARN, Theme::BG);
                snprintf(line, sizeof(line), "resistance ~%lu kOhm", (unsigned long)(lastOhm / 1000));
                c.drawString(line, x0 + 10, y0 + 56);
            } else {
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString("OPEN", x0 + 10, y0 + 56);
            }
            c.setTextColor(Theme::TEXT, Theme::BG);
            snprintf(line, sizeof(line), "sense %.2f V", lastV);
            c.drawString(line, x0 + 10, y0 + 84);
        }

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(running ? "rot: re-measure   click: stop" : "click: start measuring", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            running = !running;
            if (running) measure();
            Features::requestRedraw();
            return true;
        }
        if (in.rotationSteps != 0 && running) {
            measure();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void onExit() { running = false; }
}

REGISTER_FEATURE("Continuity", "Tools", MwIcon::Screwdriver, 40,
                 nullptr, input, nullptr, draw, onExit);
