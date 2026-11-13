// =====================================================================
//  Wi-Fi: Hosts — what's on the network, two phases:
//    1) ARP probe of the gateway /24 (1-byte UDP datagrams make lwIP
//       resolve every address; answered ARP = live host)
//    2) TCP connect to ports 80/443/8080 ONLY on the live hosts
//  So "0 hosts with web ports" no longer means "nothing found": the
//  live-host list is shown regardless of what ports they run.
//  Sliced per tick - no UI blocking. Rotation: scroll, click: rescan.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>

namespace {
    enum State : uint8_t { Idle, Probing, Resolving, PortScan, Done };
    State state = Idle;
    uint32_t baseIp = 0;
    int curHost = 1;
    int curPort = 0;
    int aliveIdx = 0;
    const uint16_t PORTS[3] = {80, 443, 8080};
    struct Host { uint8_t last; uint8_t mac[6]; uint8_t portBits; };
    Host alive[32];
    int aliveCount = 0;
    int withPorts = 0;
    char statusMsg[40] = "";
    int probeSock = -1;

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
            timeval tv = {0, 20000}; // 20 ms per attempt
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
        if (probeSock < 0) {
            probeSock = socket(AF_INET, SOCK_DGRAM, 0);
            if (probeSock < 0) { snprintf(statusMsg, sizeof(statusMsg), "socket failed"); state = Idle; return; }
            int nonblock = 1;
            fcntl(probeSock, F_SETFL, O_NONBLOCK, nonblock);
        }
        IPAddress gw = WiFi.gatewayIP();
        baseIp = ((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16) | ((uint32_t)gw[2] << 8);
        curHost = 1;
        aliveCount = 0;
        withPorts = 0;
        state = Probing;
    }

    void probeOne(int host) {
        uint32_t ip = baseIp | (uint32_t)host;
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(9);
        sa.sin_addr.s_addr = ip;
        sendto(probeSock, "x", 1, 0, (sockaddr*)&sa, sizeof(sa));
    }

    void resolveOne(int host) {
        if (aliveCount >= 32) return;
        ip4_addr_t ipa;
        IP4_ADDR(&ipa, (baseIp >> 24) & 0xFF, (baseIp >> 16) & 0xFF, (baseIp >> 8) & 0xFF, host);
        struct eth_addr* eret = nullptr;
        const ip4_addr_t* iret = nullptr;
        if (etharp_find_addr(netif_default, &ipa, &eret, &iret) < 0) return;
        int idx = aliveCount++;
        alive[idx].last = (uint8_t)host;
        memcpy(alive[idx].mac, eret, 6);
        alive[idx].portBits = 0;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "HOST SCAN");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("phase 1: who is alive (ARP)", x0 + 10, y0 + 24);
            c.drawString("phase 2: web ports on live only", x0 + 10, y0 + 36);
            c.setTextColor(statusMsg[0] ? Theme::WARN : Theme::ACCENT, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "click: start (needs Wi-Fi STA)", x0 + 10, y0 + 56);
            return;
        }
        if (state != Done) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(state == Probing ? "phase 1: ARP probe /24..." :
                         state == Resolving ? "phase 1: resolving MACs..." : "phase 2: web ports...",
                         x0 + 10, y0 + 24);
            snprintf(line, sizeof(line), "host %d/254  alive %d  web %d",
                     curHost, aliveCount, withPorts);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 40);
            c.fillRect(x0 + 10, y0 + h - 12, (int)((w - 20) * curHost / 254.0f), 3, Theme::ACCENT);
            return;
        }

        snprintf(line, sizeof(line), "%d live hosts, %d with web ports", aliveCount, withPorts);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 24);
        int rows = (h - 52) / Theme::ROW_LIST;
        for (int i = 0; i < rows && i < aliveCount; i++) {
            char ports[16];
            ports[0] = 0;
            if (alive[i].portBits & 1) strcat(ports, "80 ");
            if (alive[i].portBits & 2) strcat(ports, "443 ");
            if (alive[i].portBits & 4) strcat(ports, "8080");
            if (!ports[0]) strcpy(ports, "no web");
            snprintf(line, sizeof(line), "%u.%u.%u.%u  %s",
                     (unsigned)((baseIp >> 24) & 0xFF), (unsigned)((baseIp >> 16) & 0xFF),
                     (unsigned)((baseIp >> 8) & 0xFF), (unsigned)alive[i].last, ports);
            int y = y0 + 42 + i * Theme::ROW_LIST + 4;
            c.setTextColor(alive[i].portBits ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y);
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
        static uint32_t settleAt = 0;
        if (state == Probing) {
            for (int k = 0; k < 8 && curHost <= 254; k++, curHost++) probeOne(curHost);
            if (curHost > 254) {
                curHost = 1;
                state = Resolving;
                settleAt = millis() + 600;
            }
            Features::requestRedraw();
        } else if (state == Resolving) {
            if (millis() < settleAt) return;
            for (int k = 0; k < 8 && curHost <= 254; k++, curHost++) resolveOne(curHost);
            if (curHost > 254) {
                curHost = 0; // alive[] cursor
                state = PortScan;
            }
            Features::requestRedraw();
        } else if (state == PortScan) {
            // web ports only on hosts that answered ARP
            for (int budget = 0; budget < 6 && curHost < aliveCount; budget++) {
                uint32_t ip = baseIp | (uint32_t)alive[curHost].last;
                if (portOpen(ip, PORTS[curPort])) {
                    if (alive[curHost].portBits == 0) withPorts++;
                    alive[curHost].portBits |= (1 << curPort);
                }
                curPort++;
                if (curPort >= 3) {
                    curPort = 0;
                    curHost++;
                }
            }
            if (curHost >= aliveCount) {
                curHost = aliveCount;
                state = Done;
            }
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (probeSock >= 0) { closesocket(probeSock); probeSock = -1; }
    }
}

REGISTER_FEATURE("Hosts", MwCat::Wifi, MwIcon::Terminal, 20,
                 nullptr, input, tick, draw, onExit);
