// =====================================================================
//  Tools: Bus Pirate — I2C device scanner on bus 8/18.
//  Click — scan addresses 0x08-0x77 and show the ones found.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <Wire.h>

namespace {
    bool scanned = false;
    uint8_t found[32];
    int foundN = 0;

    void scan() {
        foundN = 0;
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        for (uint8_t addr = 0x08; addr <= 0x77 && foundN < 32; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) found[foundN++] = addr;
        }
        scanned = true;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "BUS PIRATE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!scanned) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("I2C bus 8/18", x0 + 10, y0 + 26);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: scan 0x08-0x77", x0 + 10, y0 + 46);
            return;
        }
        char line[40];
        snprintf(line, sizeof(line), "%d device(s) found", foundN);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 24);
        for (int i = 0; i < foundN && i < 12; i++) {
            snprintf(line, sizeof(line), "  0x%02X", found[i]);
            int col = i % 6, row = i / 6;
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(line, x0 + 12 + col * 44, y0 + 46 + row * Theme::ROW_LIST);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: rescan", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) { scan(); Features::requestRedraw(); return true; }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Bus Pirate", "Tools", MwIcon::Screwdriver, 20,
                 nullptr, input, tick, draw, nullptr);
