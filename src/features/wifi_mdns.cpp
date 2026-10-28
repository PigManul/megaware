// =====================================================================
//  Wi-Fi: mDNS — какие сервисы объявлены в сети (_http._tcp: принтеры,
//  камеры, NAS, AirPlay...). Клик — опрос (~3-4 c, блокирует).
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <ESPmDNS.h>

namespace {
    enum State : uint8_t { Idle, Done };
    State state = Idle;
    constexpr int MAX_SVC = 10;
    char svcName[MAX_SVC][28];
    char svcAddr[MAX_SVC][24];
    uint16_t svcPort[MAX_SVC];
    int svcCount = 0;
    char statusMsg[32] = "";

    void query() {
        // mDNS — LAN-протокол: работает и в AP-режиме (запрос мультикастом
        // по всем интерфейсам). Ретрай: единичный запрос часто пустой,
        // пока кэш соседей/ARP не собрался.
        if (!WifiMgr::isConnected() && !WifiMgr::isAPRunning()) {
            snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi STA or AP");
            state = Done;
            return;
        }
        svcCount = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            int n = MDNS.queryService("http", "tcp"); // блокирует ~3-4 c
            if (n > MAX_SVC) n = MAX_SVC;
            for (int i = 0; i < n; i++) {
                snprintf(svcName[i], sizeof(svcName[i]), "%s", MDNS.hostname(i).c_str());
                snprintf(svcAddr[i], sizeof(svcAddr[i]), "%s", MDNS.IP(i).toString().c_str());
                svcPort[i] = MDNS.port(i);
            }
            svcCount = n;
            if (svcCount > 0) break;
            delay(300);
        }
        state = Done;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "mDNS _http._tcp");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("browse LAN services (click)", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("query blocks ~4 s", x0 + 10, y0 + 40);
            return;
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + 24);
            return;
        }
        if (svcCount == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no _http services found", x0 + 10, y0 + 24);
        }
        int rows = (h - 40) / Theme::ROW_LIST;
        for (int i = 0; i < rows && i < svcCount; i++) {
            char line[72];
            snprintf(line, sizeof(line), "%s", svcName[i]);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 12, y0 + 26 + i * Theme::ROW_LIST);
            char line2[48];
            snprintf(line2, sizeof(line2), "  %s:%u", svcAddr[i], (unsigned)svcPort[i]);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line2, x0 + 12, y0 + 26 + i * Theme::ROW_LIST + 12 > y0 + h - 20 ? y0 + h - 20 : y0 + 26 + i * Theme::ROW_LIST + 12);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: re-query", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            query();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("mDNS", MwCat::Wifi, MwIcon::None, 40,
                 nullptr, input, tick, draw, nullptr);
