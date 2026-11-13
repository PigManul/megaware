// =====================================================================
//  Wi-Fi: Traceroute — REAL route trace to 8.8.8.8.
//  UDP probes with a growing TTL (1..12); routers on the path reply
//  ICMP Time Exceeded, the destination replies ICMP Port Unreachable.
//  A raw ICMP socket receives the replies (src address = the hop).
//  Sliced per tick (~10 ms poll per slice): no UI blocking.
//  Purpose: shows the router path packets take to the internet - hop 1
//  is always your gateway, the last hop is the destination.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/prot/ip.h> // IP_PROTO_ICMP

namespace {
    enum State : uint8_t { Idle, Running, Done };
    State state = Idle;
    constexpr int MAX_HOPS = 12;
    struct Hop { uint32_t ip; int32_t ms; bool reached; bool timeout; };
    Hop hops[MAX_HOPS];
    int curTtl = 0;
    uint32_t startedAt = 0;
    char statusMsg[40] = "";

    int icmpSock = -1;

    bool sendProbe(int ttl, uint32_t destNet) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) return false;
        int ttlVal = ttl;
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttlVal, sizeof(ttlVal));
        int nonblock = 1;
        fcntl(s, F_SETFL, O_NONBLOCK, nonblock);
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(33434 + ttl);
        sa.sin_addr.s_addr = destNet;
        sendto(s, "megaware", 8, 0, (sockaddr*)&sa, sizeof(sa));
        closesocket(s);
        return true;
    }

    void startTrace() {
        if (!WifiMgr::isConnected()) { snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi"); return; }
        statusMsg[0] = 0;
        if (icmpSock < 0) {
            icmpSock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
            if (icmpSock < 0) {
                snprintf(statusMsg, sizeof(statusMsg), "raw sockets unavailable");
                return;
            }
            int nonblock = 1;
            fcntl(icmpSock, F_SETFL, O_NONBLOCK, nonblock);
        }
        memset(hops, 0, sizeof(hops));
        curTtl = 1;
        startedAt = millis();
        sendProbe(1, inet_addr("8.8.8.8"));
        state = Running;
    }

    // poll the ICMP socket briefly; processes Time Exceeded / Port Unreach
    void pollIcmp() {
        if (icmpSock < 0 || curTtl == 0) return;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(icmpSock, &rd);
        timeval tv = {0, 8000}; // 8 ms slice
        if (select(icmpSock + 1, &rd, nullptr, nullptr, &tv) <= 0) return;
        uint8_t buf[256];
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        int n = recvfrom(icmpSock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
        if (n < 21) return;
        uint8_t type = buf[20];
        if (type != 3 && type != 11) return; // not unreachable / not time exceeded
        Hop& h = hops[curTtl - 1];
        h.ip = from.sin_addr.s_addr; // network order
        h.ms = (int32_t)(millis() - startedAt);
        if (type == 3) { h.reached = true; curTtl = 0; state = Done; return; }
        // next hop
        if (curTtl >= MAX_HOPS) { curTtl = 0; state = Done; return; }
        curTtl++;
        startedAt = millis();
        sendProbe(curTtl, inet_addr("8.8.8.8"));
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TRACEROUTE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("router path to 8.8.8.8:", x0 + 10, y0 + 24);
            c.drawString("hop 1 = your gateway, the", x0 + 10, y0 + 36);
            c.drawString("last hop = the destination", x0 + 10, y0 + 48);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: start (needs Wi-Fi)", x0 + 10, y0 + 70);
            if (statusMsg[0]) {
                c.setTextColor(Theme::ERR, Theme::BG);
                c.drawString(statusMsg, x0 + 10, y0 + h - 28);
            }
            return;
        }

        int shown = 0;
        for (int i = 0; i < MAX_HOPS && shown < 8; i++) {
            bool inflight = (state == Running && i == curTtl - 1);
            if (hops[i].ip == 0 && !inflight && !hops[i].timeout) continue;
            int y = y0 + 22 + shown * Theme::ROW_LIST;
            uint32_t ip = hops[i].ip;
            if (ip == 0) {
                snprintf(line, sizeof(line), "%2d  %s", i + 1,
                         inflight ? "waiting..." : "* * * (no reply)");
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            } else {
                // IPv4 in network order: print big-endian (human reading)
                snprintf(line, sizeof(line), "%2d  %u.%u.%u.%u  %ld ms%s",
                         i + 1,
                         (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
                         (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF),
                         (long)hops[i].ms,
                         hops[i].reached ? "  DST" : "");
                c.setTextColor(hops[i].reached ? Theme::OK : Theme::TEXT, Theme::BG);
            }
            c.drawString(line, x0 + 10, y);
            shown++;
        }
        if (state == Running) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("tracing (TTL in flight)...", x0 + 10, y0 + h - 28);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 28);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(state == Running ? "passive waiting..." : "click: retrace", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (state != Running) startTrace();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Running) return;
        pollIcmp();
        // per-hop timeout: 1.5 s without a reply -> mark and move on
        if (curTtl > 0 && millis() - startedAt > 1500) {
            hops[curTtl - 1].timeout = true;
            if (curTtl >= MAX_HOPS) { curTtl = 0; state = Done; }
            else { curTtl++; startedAt = millis(); sendProbe(curTtl, inet_addr("8.8.8.8")); }
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (icmpSock >= 0) { closesocket(icmpSock); icmpSock = -1; }
    }
}

REGISTER_FEATURE("Traceroute", MwCat::Wifi, MwIcon::Others, 35,
                 nullptr, input, tick, draw, onExit);
