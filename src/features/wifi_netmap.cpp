// =====================================================================
//  Wi-Fi: Network Map — REAL ARP mapper. Probes every address of the
//  gateway /24 with a 1-byte UDP datagram (lwIP then issues an ARP
//  request for it), then reads the resolved MACs back via
//  etharp_find_addr: an answered ARP = a live host, no port assumptions.
//  Sliced per tick (8 probes / 8 lookups each): no UI blocking.
//  Shows which hosts are NEW since the previous scan.
//  Rotation: scroll, click: rescan.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>

namespace {
    enum State : uint8_t { Idle, Probing, Resolving, Done };
    State state = Idle;
    uint32_t baseIp = 0;
    int curHost = 1;
    struct Host { uint8_t last; uint8_t mac[6]; uint8_t isNew; };
    Host found[32];
    int foundCount = 0;
    int prevCount = 0;
    uint8_t prevHosts[32];
    char statusMsg[40] = "";
    int probeSock = -1;

    void startScan() {
        if (!WifiMgr::isConnected()) {
            snprintf(statusMsg, sizeof(statusMsg), "connect to Wi-Fi first");
            return;
        }
        if (probeSock < 0) {
            probeSock = socket(AF_INET, SOCK_DGRAM, 0);
            if (probeSock < 0) {
                snprintf(statusMsg, sizeof(statusMsg), "socket failed");
                return;
            }
            int nonblock = 1;
            fcntl(probeSock, F_SETFL, O_NONBLOCK, nonblock);
        }
        IPAddress gw = WiFi.gatewayIP();
        baseIp = ((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16) | ((uint32_t)gw[2] << 8);
        prevCount = foundCount;
        for (int i = 0; i < foundCount && i < 32; i++) prevHosts[i] = found[i].last;
        curHost = 1;
        foundCount = 0;
        state = Probing;
    }

    // UDP probe: lwIP resolves the destination MAC via ARP on send -
    // a live host answers the ARP request, dead IP stays unresolved.
    void probeOne(int host) {
        uint32_t ip = baseIp | (uint32_t)host;
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(9); // discard port, payload ignored
        sa.sin_addr.s_addr = ip;
        sendto(probeSock, "x", 1, 0, (sockaddr*)&sa, sizeof(sa));
    }

    void resolveOne(int host) {
        if (foundCount >= 32) return;
        ip4_addr_t ipa;
        IP4_ADDR(&ipa, (baseIp >> 24) & 0xFF, (baseIp >> 16) & 0xFF, (baseIp >> 8) & 0xFF, host);
        struct eth_addr* eret = nullptr;
        const ip4_addr_t* iret = nullptr;
        if (etharp_find_addr(netif_default, &ipa, &eret, &iret) < 0) return;
        int idx = foundCount++;
        found[idx].last = (uint8_t)host;
        memcpy(found[idx].mac, eret, 6);
        bool wasPrev = false;
        for (int p = 0; p < prevCount; p++) if (prevHosts[p] == host) { wasPrev = true; break; }
        found[idx].isNew = wasPrev ? 0 : 1;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NETWORK MAP (ARP)");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("finds ALL live hosts by ARP", x0 + 10, y0 + 24);
            c.drawString("(no port assumptions)", x0 + 10, y0 + 36);
            c.setTextColor(statusMsg[0] ? Theme::WARN : Theme::TEXT_FAINT, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "click: start (needs Wi-Fi STA)", x0 + 10, y0 + 56);
            return;
        }
        if (state == Probing || state == Resolving) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(state == Probing ? "ARP-probing 192.168.x.1-254..." : "resolving MACs...", x0 + 10, y0 + 24);
            char line[40];
            snprintf(line, sizeof(line), "host %d/254  alive %d", curHost, foundCount);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 42);
            c.fillRect(x0 + 10, y0 + h - 12, (int)((w - 20) * curHost / 254.0f), 3, Theme::ACCENT);
            return;
        }

        char line[48];
        snprintf(line, sizeof(line), "%u.%u.%u.0/24 - %d live hosts",
                 (unsigned)((baseIp >> 24) & 0xFF), (unsigned)((baseIp >> 16) & 0xFF),
                 (unsigned)((baseIp >> 8) & 0xFF), foundCount);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 24);

        int rows = (h - 52) / Theme::ROW_LIST;
        for (int i = 0; i < rows && i < foundCount; i++) {
            char row[64];
            snprintf(row, sizeof(row), "%u.%u.%u.%u  %02X:%02X:%02X:%02X:%02X:%02X",
                     (unsigned)((baseIp >> 24) & 0xFF), (unsigned)((baseIp >> 16) & 0xFF),
                     (unsigned)((baseIp >> 8) & 0xFF), (unsigned)found[i].last,
                     found[i].mac[0], found[i].mac[1], found[i].mac[2],
                     found[i].mac[3], found[i].mac[4], found[i].mac[5]);
            int y = y0 + 42 + i * Theme::ROW_LIST + 4;
            c.setTextColor(found[i].isNew ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
            c.drawString(row, x0 + 10, y);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("yellow = new since last scan", x0 + 10, y0 + h - 26);
        c.drawString("click: rescan (diff vs prev)", x0 + 10, y0 + h - 14);
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
                settleAt = millis() + 600; // let the last ARP replies land
            }
            Features::requestRedraw();
        } else if (state == Resolving) {
            if (millis() < settleAt) return;
            for (int k = 0; k < 8 && curHost <= 254; k++, curHost++) resolveOne(curHost);
            if (curHost > 254) { state = Done; settleAt = 0; }
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (probeSock >= 0) { closesocket(probeSock); probeSock = -1; }
    }
}

REGISTER_FEATURE("Network Map", MwCat::Wifi, MwIcon::Ethernet, 15,
                 nullptr, input, tick, draw, onExit);
