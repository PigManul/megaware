// =====================================================================
//  Wi-Fi: Analyzer — гистограмма эфира по каналам 1..13: число сетей
//  и пик RSSI на канал. Клик — рескан. Только пассивный приём.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"

namespace {
    bool scanRequested = false;

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "WIFI ANALYZER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);

        if (WifiMgr::isScanning()) {
            c.setTextDatum(textdatum_t::top_left);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("SCANNING...", x0 + 10, y0 + 22);
            return;
        }

        int n = WifiMgr::resultCount();
        int cnt[14] = {0};
        int peak[14] = {0};
        for (int i = 0; i < n && i < 40; i++) {
            WifiNetwork net = WifiMgr::result(i);
            int ch = net.channel;
            if (ch < 1 || ch > 13) continue;
            cnt[ch]++;
            if (net.rssi > peak[ch]) peak[ch] = net.rssi;
        }
        int maxCnt = 1;
        for (int ch = 1; ch <= 13; ch++) if (cnt[ch] > maxCnt) maxCnt = cnt[ch];

        int bw = (w - 16) / 13;
        int baseY = y0 + h - 16;
        c.setTextDatum(textdatum_t::middle_center);
        for (int ch = 1; ch <= 13; ch++) {
            int cx = x0 + 8 + (ch - 1) * bw;
            int bh = (int)((float)cnt[ch] / maxCnt * (h - 66));
            if (cnt[ch]) c.fillRect(cx, baseY - bh, bw - 2, bh, Theme::ACCENT);
            char num[4];
            snprintf(num, sizeof(num), "%d", ch);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(num, cx + bw / 2, baseY + 8);
            if (cnt[ch]) {
                char p[8];
                snprintf(p, sizeof(p), "%d", (int)peak[ch]);
                c.setTextColor(Theme::TEXT, Theme::BG);
                c.drawString(p, cx + bw / 2, baseY - bh - 5);
            }
        }

        c.setTextDatum(textdatum_t::top_left);
        char line[48];
        snprintf(line, sizeof(line), "%d networks  peak dBm above bars", n);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 22);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: rescan", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            WifiMgr::startScan();
            scanRequested = true;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (scanRequested && !WifiMgr::isScanning()) {
            scanRequested = false;
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Analyzer", MwCat::Wifi, MwIcon::Pulse, 10,
                 nullptr, input, tick, draw, nullptr);
