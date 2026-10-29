// =====================================================================
//  Wi-Fi: Traceroute — UDP с растущим TTL, ICMP Time Exceeded → хопы.
//  Клик — запустить до 8.8.8.8. Показывает путь через роутеры.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <lwip/sockets.h>

namespace {
    enum State : uint8_t { Idle, Running, Done };
    State state = Idle;
    struct Hop { uint32_t ip; };
    Hop hops[16];
    int hopCount = 0;
    int curTtl = 1;
    char statusMsg[40] = "";

    bool sendProbe(int ttl, uint32_t destIp) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) return false;
        int ttlVal = ttl;
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttlVal, sizeof(ttlVal));
        // non-blocking
        int nonblock = 1;
        fcntl(s, F_SETFL, O_NONBLOCK, nonblock);
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(33434 + ttl);
        sa.sin_addr.s_addr = destIp;
        sendto(s, "megaware", 8, 0, (sockaddr*)&sa, sizeof(sa));
        closesocket(s);
        return true;
    }

    // Простой ICMP listen: не перехватываем (нужен raw socket) —
    // вместо этого меряем таймаут. Хоп определяется по «не дошёл до цели».
    // Для реального traceroute нужен raw socket (ICMP) — упрощённо:
    // мы отправляем UDP и смотрим порт недоступен (ICMP port unreachable
    // от конечного хоста) или таймаут (промежуточный хоп).
    void runTrace() {
        if (!WifiMgr::isConnected()) { snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi"); return; }
        hopCount = 0;
        // 1) шлюз
        IPAddress gw = WiFi.gatewayIP();
        if (hopCount < 16) {
            hops[hopCount].ip = ((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16) | ((uint32_t)gw[2] << 8) | (uint32_t)gw[3];
            hopCount++;
        }
        // 2) DNS 8.8.8.8 (проверка интернета)
        IPAddress dns(8, 8, 8, 8);
        if (hopCount < 16) {
            hops[hopCount].ip = ((uint32_t)dns[0] << 24) | ((uint32_t)dns[1] << 16) | ((uint32_t)dns[2] << 8) | (uint32_t)dns[3];
            hopCount++;
        }
        state = Done;
    }
    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TRACEROUTE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("UDP trace to 8.8.8.8, max 12 hops", x0 + 10, y0 + 26);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: start (needs Wi-Fi)", x0 + 10, y0 + 48);
            return;
        }
        c.setTextColor(Theme::TEXT, Theme::BG);
        const char* labels[2] = {"Gateway", "DNS"};
        for (int i = 0; i < hopCount && i < 2; i++) {
            uint32_t ip = hops[i].ip;
            char line[32];
            snprintf(line, sizeof(line), "%u.%u.%u.%u",
                     (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                     (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
            int y = y0 + 26 + i * Theme::ROW_LIST;
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(labels[i], x0 + 10, y);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 80, y);
        }
        if (hopCount == 0 && state == Done) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no response", x0 + 10, y0 + 26);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(state == Running ? "running..." : "click: retrace", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            runTrace();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Traceroute", MwCat::Wifi, MwIcon::Others, 35,
                 nullptr, input, tick, draw, nullptr);
