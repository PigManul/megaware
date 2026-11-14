// =====================================================================
//  Wi-Fi: SOCKS4 proxy on port 1080 (CONNECT only, + SOCKS4a hostnames).
//  Same two-way relay engine as the TCP Bridge: client <-> upstream
//  byte streams, up to 3 simultaneous connections. Click: start/stop.
//  Router/SOCKS5 note: SOCKS5 (auth + UDP) is a bigger protocol - not
//  implemented; SOCKS4/4a covers plain TCP tunneling.
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>

namespace {
    bool running = false;
    WiFiServer* srv = nullptr;
    uint32_t connsTotal = 0;
    uint32_t relayBytes = 0;
    char statusMsg[40] = "";

    // ---- per-connection state machine ----
    struct Conn {
        bool used = false;
        uint8_t phase = 0;      // 0 greeting, 1 relaying
        WiFiClient sideA;
        WiFiClient sideB;
        uint8_t gbuf[16];       // ver/cmd/port/ip
        size_t glen = 0;
        char user[33];          // userid / hostname accumulation
        size_t ulen = 0;
        uint16_t rport = 0;
        uint32_t ip = 0;
    };
    Conn conn[3];

    void closeConn(Conn& c) {
        c.sideA.stop();
        c.sideB.stop();
        c.used = false;
        c.phase = 0;
        c.glen = 0;
        c.ulen = 0;
    }

    void greet(Conn& c, bool granted) {
        uint8_t r[8] = {0, (uint8_t)(granted ? 0x5A : 0x5B), 0, 0, 0, 0, 0, 0};
        c.sideA.write(r, 8);
    }

    void feed(Conn& c, uint8_t b) {
        if (c.phase == 0) {
            if (c.glen < 8) {
                c.gbuf[c.glen++] = b;
                if (c.glen == 8) {
                    if (c.gbuf[0] != 4) { greet(c, false); closeConn(c); return; }
                    c.rport = ((uint16_t)c.gbuf[2] << 8) | c.gbuf[3];
                    c.ip = ((uint32_t)c.gbuf[4] << 24) | ((uint32_t)c.gbuf[5] << 16) |
                           ((uint32_t)c.gbuf[6] << 8) | (uint32_t)c.gbuf[7];
                }
                return;
            }
            // userid until NUL
            if (b == 0) {
                // SOCKS4a: ip = 0.0.0.x -> hostname follows the userid
                bool hasHost = (c.gbuf[4] == 0 && c.gbuf[5] == 0 && c.gbuf[6] == 0 && c.gbuf[7] != 0);
                if (hasHost) {
                    c.user[c.ulen] = 0;
                    if (!c.sideB.connect(String(c.user).c_str(), c.rport)) { greet(c, false); closeConn(c); return; }
                } else {
                    if (!c.sideB.connect(IPAddress(c.ip), c.rport)) { greet(c, false); closeConn(c); return; }
                }
                greet(c, true);
                c.phase = 1;
                c.glen = 0;
                c.ulen = 0;
                connsTotal++;
                return;
            }
            if (c.ulen + 1 < sizeof(c.user)) c.user[c.ulen++] = (char)b;
            else closeConn(c); // absurdly long userid
            return;
        }
        // phase 1: client -> upstream
        c.sideB.write(b);
        relayBytes++;
    }

    void pump() {
        WiFiClient nc = srv->accept();
        if (nc) {
            int slot = -1;
            for (auto& c : conn) if (!c.used) { slot = 1; c.used = true; c.phase = 0; c.sideA = nc; break; }
            if (slot < 0) nc.stop();
        }
        for (auto& c : conn) {
            if (!c.used) continue;
            // greeting bytes
            while (c.phase == 0 && c.sideA.available()) feed(c, (uint8_t)c.sideA.read());
            if (!c.used) continue;
            // relay both directions
            uint8_t buf[192];
            int avail;
            while ((avail = c.sideA.available()) > 0 && c.used) {
                int n = c.sideA.read(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
                if (n <= 0 || c.sideB.write(buf, n) != n) { closeConn(c); break; }
                relayBytes += n;
            }
            if (!c.used) continue;
            while ((avail = c.sideB.available()) > 0 && c.used) {
                int n = c.sideB.read(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
                if (n <= 0 || c.sideA.write(buf, n) != n) { closeConn(c); break; }
                relayBytes += n;
            }
            if (c.used && (!c.sideA.connected() || !c.sideB.connected())) {
                if (!c.sideA.available() && !c.sideB.available()) closeConn(c);
            }
        }
    }

    void start() {
        if (!WifiMgr::isConnected()) {
            snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi STA");
            return;
        }
        srv = new WiFiServer(1080);
        srv->begin();
        srv->setNoDelay(true);
        running = true;
        statusMsg[0] = 0;
    }

    void stop() {
        for (auto& c : conn) if (c.used) closeConn(c);
        if (srv) { srv->stop(); delete srv; srv = nullptr; }
        running = false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "SOCKS4 PROXY");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        if (!running) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("SOCKS4/4a proxy, port 1080", x0 + 10, y0 + 24);
            c.drawString("CONNECT only, plain TCP", x0 + 10, y0 + 36);
            c.setTextColor(statusMsg[0] ? Theme::ERR : Theme::ACCENT, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "click: start", x0 + 10, y0 + 56);
            return;
        }
        int n = 0;
        for (auto& cn : conn) if (cn.used) n++;
        c.setTextColor(Theme::OK, Theme::BG);
        snprintf(line, sizeof(line), "listening :1080, conns %d/3", n);
        c.drawString(line, x0 + 10, y0 + 24);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "total %lu  relayed %lu KB",
                 (unsigned long)connsTotal, (unsigned long)(relayBytes / 1024));
        c.drawString(line, x0 + 10, y0 + 40);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("configure the app's proxy to", x0 + 10, y0 + 58);
        snprintf(line, sizeof(line), "%s:1080", WiFi.localIP().toString().c_str());
        c.drawString(line, x0 + 10, y0 + 68);
        c.drawString("click: stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (running) stop();
            else start();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!running || !srv) return;
        pump();
        static uint32_t lastUi = 0;
        if (millis() - lastUi > 500) { lastUi = millis(); Features::requestRedraw(); }
    }

    void onExit() { if (running) stop(); }
}

REGISTER_FEATURE("SOCKS4", MwCat::Wifi, MwIcon::Others, 55,
                 nullptr, input, tick, draw, onExit);
