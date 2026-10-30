// =====================================================================
//  Wi-Fi: Hosts — карта своей сети: TCP connect-scan шлюза /24 по
//  портам 80/443/8080 (неблокирующие сокеты, ~10 адресов за тик —
//  интерфейс не замирает). Требует подключения к сети (STA).
//  Клик — начать скан / повторить.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <lwip/sockets.h>

namespace {
    enum State : uint8_t { Idle, Scanning, Done };
    State state = Idle;
    uint32_t baseIp = 0;           // первые 3 октета шлюза, сетевой порядок
    int curHost = 1;               // 1..254
    int curPort = 0;
    int scanTickBudget = 0;
    const uint16_t PORTS[3] = {80, 443, 8080};
    struct Host { uint8_t last; uint8_t portBits; };
    Host found[32];
    int foundCount = 0;
    char statusMsg[40] = "";

    bool portOpen(uint32_t ipNet, uint16_t port) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        int nonblock = 1;
        fcntl(s, F_SETFL, O_NONBLOCK, nonblock);
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = ipNet;
        int r = connect(s, (sockaddr*)&sa, sizeof(sa));
        bool open = (r == 0);
        if (!open && errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            timeval tv = {0, 20000}; // 20 мс на попытку
            if (select(s + 1, nullptr, &wset, nullptr, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                open = (err == 0);
            }
        }
        closesocket(s);
        return open;
    }

    void startScan() {
        if (!WifiMgr::isConnected()) {
            snprintf(statusMsg, sizeof(statusMsg), "connect to Wi-Fi first");
            state = Idle;
            return;
        }
        IPAddress gw = WiFi.gatewayIP();
        baseIp = ((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16) | ((uint32_t)gw[2] << 8);
        curHost = 1;
        curPort = 0;
        foundCount = 0;
        state = Scanning;
    }

    void tickScan() {
        for (int budget = 0; budget < 10 && state == Scanning; budget++) {
            uint32_t ip = baseIp | (uint32_t)curHost;
            if (portOpen(ip, PORTS[curPort])) {
                int idx = -1;
                for (int i = 0; i < foundCount; i++) if (found[i].last == curHost) { idx = i; break; }
                if (idx < 0 && foundCount < 32) {
                    idx = foundCount++;
                    found[idx].last = curHost;
                    found[idx].portBits = 0;
                }
                if (idx >= 0) found[idx].portBits |= (1 << curPort);
            }
            curPort++;
            if (curPort >= 3) {
                curPort = 0;
                curHost++;
                if (curHost > 254) state = Done;
            }
        }
        Features::requestRedraw();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "HOST SCAN");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Idle) {
            c.setTextColor(statusMsg[0] ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "scan gateway /24: ports 80/443/8080", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: start (needs Wi-Fi STA)", x0 + 10, y0 + h - 14);
            return;
        }
        if (state == Scanning) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("SCANNING...", x0 + 10, y0 + 24);
            char line[40];
            snprintf(line, sizeof(line), "host %d/254  found %d", curHost, foundCount);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 40);
            c.fillRect(x0 + 10, y0 + h - 12, (int)((w - 20) * curHost / 254.0f), 3, Theme::ACCENT);
            return;
        }

        char line[48];
        snprintf(line, sizeof(line), "gateway /24 — %d hosts with web ports", foundCount);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 24);
        int rows = (h - 52) / Theme::ROW_LIST;
        for (int i = 0; i < rows && i < foundCount; i++) {
            char ipbuf[24];
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                     (unsigned)((baseIp >> 24) & 0xFF), (unsigned)((baseIp >> 16) & 0xFF),
                     (unsigned)((baseIp >> 8) & 0xFF), (unsigned)found[i].last);
            char ports[16];
            ports[0] = 0;
            if (found[i].portBits & 1) strcat(ports, "80 ");
            if (found[i].portBits & 2) strcat(ports, "443 ");
            if (found[i].portBits & 4) strcat(ports, "8080");
            char row[64];
            snprintf(row, sizeof(row), "%-15s %s", ipbuf, ports);
            int y = y0 + 42 + i * Theme::ROW_LIST + 4;
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(row, x0 + 10, y);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: rescan", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            startScan();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state == Scanning) tickScan();
    }
}

REGISTER_FEATURE("Hosts", MwCat::Wifi, MwIcon::Terminal, 20,
                 nullptr, input, tick, draw, nullptr);
