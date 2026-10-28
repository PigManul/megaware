// =====================================================================
//  Bluetooth: BLE Scan — список устройств поблизости, сортировка по
//  RSSI, маркер [F] у Apple Find My-трекеров. Клик — новый скан (3 c,
//  блокирует). Совет: Release Wi-Fi RAM перед сканом (Settings -> Wi-Fi).
// =====================================================================
#include "../ui/features.h"
#include "../hal/blescan.h"
#include "../../include/theme.h"

namespace {
    bool scanned = false;

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "BLE SCAN");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (ESP.getFreeHeap() < 70000 && !scanned) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("low heap — release Wi-Fi RAM", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("(Settings -> Wi-Fi), then click", x0 + 10, y0 + 40);
            return;
        }

        int n = BleScan::count();
        char line[56];
        snprintf(line, sizeof(line), "%d devices  %d find-my", n, BleScan::findMyCount());
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 22);

        int rows = (h - 38) / Theme::ROW_LIST;
        for (int i = 0; i < rows && i < n; i++) {
            const BleScan::Entry& e = BleScan::at(i);
            char row[76];
            snprintf(row, sizeof(row), "%s%d %s%s",
                     e.findMy ? "[F] " : "", e.rssi,
                     e.name[0] ? e.name : e.addr,
                     e.findMy ? " !" : "");
            int y = y0 + 36 + i * Theme::ROW_LIST;
            c.setTextColor(e.findMy ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
            c.drawString(row, x0 + 12, y);
        }
        if (!scanned && n == 0) {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: scan (3 s)", x0 + 10, y0 + h - 14);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: rescan", x0 + 10, y0 + h - 14);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            scanned = BleScan::scanBlocking(3);
            BleScan::deinit(); // результаты в статике — стек выгружаем, heap свободен
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("BLE Scan", "Others", MwIcon::Bluetooth, 10,
                 nullptr, input, tick, draw, nullptr);
