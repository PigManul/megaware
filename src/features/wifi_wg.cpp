// =====================================================================
//  Wi-Fi: WireGuard client — VPN tunnel for the board's own traffic.
//  Uses ciniml/WireGuard-ESP32-Arduino (wireguard-lwip port, mbedtls
//  based). Configuration lives in NVS and is set from the WebUI
//  terminal BEFORE bringing the tunnel up:
//    wg priv <base64 private key>
//    wg peer <base64 peer public key>
//    wg ep <endpoint host or ip>
//    wg port <endpoint port>
//    wg ip <tunnel local ip>  wg gw <tunnel gateway ip>
//    wg ain <allowed ip>  wg amask <allowed mask bits>
//  Click: tunnel up/down. NOTE: the tunnel netif is separate - traffic
//  to AllowedIPs goes through it when apps bind to the tunnel IP.
// =====================================================================
#include "../ui/features.h"
#include "../net/webui.h"
#include "../net/wifi_manager.h"
#include "../hal/config.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <WireGuard-ESP32.h>

namespace {
    bool up = false;
    char statusMsg[40] = "";
    uint32_t upAt = 0;

    WireGuard wg;

    bool fieldOk(int idx) {
        const char* keys[8] = {"wg_priv", "wg_peer", "wg_ep", "wg_port", "wg_ip", "wg_gw", "wg_ain", "wg_amask"};
        return Config::getString(keys[idx], "").length() > 0;
    }

    bool bringUp() {
        if (!WifiMgr::isConnected()) { snprintf(statusMsg, sizeof(statusMsg), "need Wi-Fi STA"); return false; }
        if (!fieldOk(0) || !fieldOk(1) || !fieldOk(2) || !fieldOk(4)) {
            snprintf(statusMsg, sizeof(statusMsg), "config missing (wg priv/peer/ep/ip)");
            return false;
        }
        String priv = Config::getString("wg_priv", "");
        String peer = Config::getString("wg_peer", "");
        String ep = Config::getString("wg_ep", "");
        int port = Config::getInt("wg_port", 51820);
        String lip = Config::getString("wg_ip", "");

        IPAddress lIp; lIp.fromString(lip);
        if (!wg.begin(lIp, priv.c_str(), ep.c_str(), peer.c_str(), (uint16_t)port)) {
            snprintf(statusMsg, sizeof(statusMsg), "wg.begin failed (keys/endpoint?)");
            return false;
        }
        up = true;
        upAt = millis();
        statusMsg[0] = 0;
        return true;
    }

    void bringDown() {
        if (up) wg.end();
        up = false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "WIREGUARD");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        const char* names[8] = {"priv", "peer", "ep", "port", "ip", "gw", "ain", "amask"};
        const char* keys[8] = {"wg_priv", "wg_peer", "wg_ep", "wg_port", "wg_ip", "wg_gw", "wg_ain", "wg_amask"};
        int y = y0 + 20;
        for (int i = 0; i < 8; i++) {
            String v = Config::getString(keys[i], "");
            if (v.length() > 24) {
                String shortV = v.substring(0, 22) + "..";
                v = shortV;
            }
            snprintf(line, sizeof(line), "%-5s %s", names[i], v.length() ? v.c_str() : "-");
            c.setTextColor(fieldOk(i) || i >= 5 ? Theme::TEXT_DIM : Theme::WARN, Theme::BG);
            c.drawString(line, x0 + 10, y);
            y += Theme::ROW_LIST;
            if (y > y0 + h - 40) break;
        }

        c.setTextColor(up ? Theme::OK : Theme::TEXT_FAINT, Theme::BG);
        if (up) snprintf(line, sizeof(line), "TUNNEL UP (%lu s)", (unsigned long)((millis() - upAt) / 1000));
        else snprintf(line, sizeof(line), "%s", statusMsg[0] ? statusMsg : "down");
        c.drawString(line, x0 + 10, y0 + h - 26);

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: up/down  cfg: 'wg ...' in terminal", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (up) bringDown();
            else bringUp();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        // config may change from the terminal while the tunnel is down
        static uint32_t lastUi = 0;
        if (millis() - lastUi > 1000) { lastUi = millis(); if (!up) Features::requestRedraw(); }
    }

    void onExit() { if (up) bringDown(); }
}

REGISTER_FEATURE("WireGuard", MwCat::Wifi, MwIcon::Lock, 75,
                 nullptr, input, tick, draw, onExit);
