// =====================================================================
//  Wi-Fi: Telnet — remote console on port 23 running the SAME command
//  engine as the WebUI terminal (help/status/ls/cat/wifi/led/...).
//  Up to 2 simultaneous clients. Click: start/stop (needs Wi-Fi).
//  Security note: no authentication - port 23 on a LAN toy, documented.
// =====================================================================
#include "../ui/features.h"
#include "../net/webui.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>

namespace {
    bool running = false;
    WiFiServer* srv = nullptr;
    char statusMsg[40] = "";
    uint32_t cmdsDone = 0;

    struct TClient {
        bool used = false;
        WiFiClient c;
        char line[160];
        size_t len = 0;
        bool greeted = false;
    };
    TClient cl[2];

    void closeCli(TClient& t) {
        t.c.stop();
        t.used = false;
        t.len = 0;
    }

    void feed(TClient& t, char ch) {
        if (ch == 0xFF) { t.len = 0; return; }          // telnet IAC: skip
        if (ch == '\r') return;
        if (ch == '\n') {
            t.line[t.len] = 0;
            if (t.len) {
                String cmd(t.line);
                cmd.trim();
                if (cmd == "reboot") {
                    t.c.print("rebooting\r\n");
                    t.c.flush();
                    delay(400);
                    ESP.restart();
                }
                String out;
                bool ok = WebUI::execCommand(cmd, out);
                t.c.printf("%s%s\r\nmega> ", ok ? "" : "ERR: ", out.c_str());
                t.c.flush();
                cmdsDone++;
            }
            t.len = 0;
            return;
        }
        if (t.len + 1 < sizeof(t.line)) t.line[t.len++] = ch;
    }

    void startTelnet() {
        if (!WifiMgr::isConnected() && !WifiMgr::isAPRunning()) {
            snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi STA or AP");
            return;
        }
        srv = new WiFiServer(23);
        srv->begin();
        srv->setNoDelay(true);
        running = true;
        statusMsg[0] = 0;
    }

    void stopTelnet() {
        for (auto& t : cl) if (t.used) closeCli(t);
        if (srv) { srv->stop(); delete srv; srv = nullptr; }
        running = false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TELNET SERVER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        if (!running) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("remote console, port 23", x0 + 10, y0 + 24);
            c.drawString("same commands as the web terminal", x0 + 10, y0 + 36);
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("no authentication (LAN toy)", x0 + 10, y0 + 52);
            c.setTextColor(statusMsg[0] ? Theme::ERR : Theme::ACCENT, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "click: start", x0 + 10, y0 + 70);
            return;
        }
        int n = 0;
        for (auto& t : cl) if (t.used) n++;
        c.setTextColor(Theme::OK, Theme::BG);
        snprintf(line, sizeof(line), "listening :23, clients %d/2", n);
        c.drawString(line, x0 + 10, y0 + 24);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "commands executed: %lu", (unsigned long)cmdsDone);
        c.drawString(line, x0 + 10, y0 + 40);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("try: help | status | ls sd /", x0 + 10, y0 + 56);
        c.drawString("click: stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (running) stopTelnet();
            else startTelnet();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!running || !srv) return;
        WiFiClient nc = srv->accept();
        if (nc) {
            int slot = -1;
            for (auto& t : cl) if (!t.used) { slot = 1; t.used = true; t.c = nc; t.len = 0; t.c.print("MEGAWARE telnet\r\nmega> "); break; }
            if (slot < 0) nc.stop();
        }
        for (auto& t : cl) {
            if (!t.used) continue;
            if (!t.c.connected()) { closeCli(t); continue; }
            while (t.c.available()) feed(t, (char)t.c.read());
        }
    }

    void onExit() { if (running) stopTelnet(); }
}

REGISTER_FEATURE("Telnet", MwCat::Wifi, MwIcon::Terminal, 45,
                 nullptr, input, tick, draw, onExit);
