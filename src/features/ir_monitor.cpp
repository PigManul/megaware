// =====================================================================
//  IR: Monitor — живой приём: наведи пульт и жми кнопки. Показывает
//  протокол, address/command, биты и счётчик пакетов. RX = GPIO1.
// =====================================================================
#include "../ui/features.h"
#include "../hal/ir.h"
#include "../../include/theme.h"

namespace {
    Ir::Packet last;
    bool have = false;
    uint32_t packets = 0;
    char valueHex[24] = "";

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "IR MONITOR");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!have) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("point a remote and press keys", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("RX: GPIO1  (passive receive)", x0 + 10, y0 + 44);
            return;
        }
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(valueHex, x0 + 10, y0 + 24);
        char line[56];
        snprintf(line, sizeof(line), "%s  %u bits", last.proto, last.bits);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 42);
        snprintf(line, sizeof(line), "addr 0x%08lX  cmd 0x%08lX",
                 (unsigned long)last.address, (unsigned long)last.command);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 58);
        snprintf(line, sizeof(line), "packets: %lu", (unsigned long)packets);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        Ir::Packet pkt;
        if (Ir::poll(pkt)) {
            last = pkt;
            have = true;
            packets++;
            snprintf(valueHex, sizeof(valueHex), "0x%llX", (unsigned long long)pkt.value);
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Monitor", "IR", MwIcon::Pulse, 10,
                 nullptr, input, tick, draw, nullptr);
