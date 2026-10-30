// =====================================================================
//  Ethernet: TCP Bridge — wired-side LAN with port forwards.
//
//  Why not kernel NAT: the prebuilt ESP32-S3 lwIP ships with
//  IP_FORWARD/IP_NAPT compiled OUT (verified: the ip4_napt object is
//  empty), and the lwIP sources are not in the framework package, so
//  kernel-level forwarding cannot be enabled without rebuilding the
//  whole SDK. The same field problem is solved at the application
//  layer instead:
//    1. DHCP server on the cable segment: clients get 192.168.169.x,
//       board = 192.168.169.1. Board services (WebUI, terminal, API)
//       are reachable from the cable - lwIP TCP PCBs are port-based,
//       not interface-based.
//    2. Port-forward rules: a listening port on the board relays TCP
//       byte streams to host:rport over the Wi-Fi uplink (e.g. cable
//       laptop -> 192.168.169.1:8000 -> router admin at 192.168.1.1:80).
//  Rules are edited from the WebUI terminal: `bridge add <lport> <host>
//  <rport>` (see /docs). Click: start/stop. Rotation: select rule.
// =====================================================================
#include "../ui/features.h"
#include "../hal/eth.h"
#include "../hal/bridge_cfg.h"
#include "../net/wifi_manager.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_netif.h>

namespace {
    bool bridgeUp = false;
    BridgeRule rules[BridgeCfg::SLOTS];
    uint32_t lastVer = 0;
    int selRow = 0;

    struct Conn {
        bool used = false;
        WiFiClient sideA; // the client that connected to the board
        WiFiClient sideB; // upstream connection over Wi-Fi
        uint32_t bytesAB = 0, bytesBA = 0;
    };
    constexpr int MAX_CONNS = 6;
    Conn conns[MAX_CONNS];
    WiFiServer* servers[BridgeCfg::SLOTS] = {nullptr, nullptr};
    uint16_t srvPort[BridgeCfg::SLOTS] = {0, 0};
    uint32_t relayBytes = 0;
    uint32_t connTotal = 0;
    char statusMsg[56] = "";

    void closeConn(Conn& cn) {
        cn.sideA.stop();
        cn.sideB.stop();
        cn.used = false;
    }

    void applyRules() {
        BridgeCfg::load(rules);
        lastVer = BridgeCfg::version();
        for (int i = 0; i < BridgeCfg::SLOTS; i++) {
            if (!rules[i].enabled || !rules[i].lport || rules[i].host[0] == 0 || !rules[i].rport) {
                if (servers[i]) { servers[i]->stop(); delete servers[i]; servers[i] = nullptr; }
                continue;
            }
            if (servers[i] && (srvPort[i] != rules[i].lport)) {
                servers[i]->stop();
                delete servers[i];
                servers[i] = nullptr;
            }
            if (!servers[i]) {
                servers[i] = new WiFiServer(rules[i].lport);
                servers[i]->begin();
                servers[i]->setNoDelay(true);
                srvPort[i] = rules[i].lport;
            }
        }
        for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used) closeConn(conns[i]);
    }

    void stopBridge() {
        for (int i = 0; i < BridgeCfg::SLOTS; i++)
            if (servers[i]) { servers[i]->stop(); delete servers[i]; servers[i] = nullptr; }
        for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used) closeConn(conns[i]);
        if (bridgeUp && EthW5500::netif()) {
            esp_netif_dhcps_stop(EthW5500::netif());
            EthW5500::stop();
        }
        bridgeUp = false;
    }

    bool startBridge() {
        if (!WifiMgr::isConnected()) {
            snprintf(statusMsg, sizeof(statusMsg), "uplink needed: connect Wi-Fi first");
            return false;
        }
        if (!EthW5500::start()) {
            strncpy(statusMsg, EthW5500::lastError(), sizeof(statusMsg) - 1);
            return false;
        }
        // static address on the cable side + DHCP server for the clients
        esp_netif_t* nif = EthW5500::netif();
        esp_netif_dhcpc_stop(nif);
        esp_netif_ip_info_t ip;
        ip.ip.addr = ESP_IP4TOADDR(192, 168, 169, 1);
        ip.gw.addr = ESP_IP4TOADDR(192, 168, 169, 1);
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        esp_netif_set_ip_info(nif, &ip);
        if (esp_netif_dhcps_start(nif) != ESP_OK) {
            snprintf(statusMsg, sizeof(statusMsg), "DHCP server failed to start");
        }
        applyRules();
        bridgeUp = true;
        statusMsg[0] = 0;
        return true;
    }

    void pump() {
        // accept new connections for enabled rules
        for (int r = 0; r < BridgeCfg::SLOTS; r++) {
            if (!servers[r] || !rules[r].enabled) continue;
            WiFiClient c = servers[r]->accept();
            if (!c) continue;
            int slot = -1;
            for (int i = 0; i < MAX_CONNS; i++) if (!conns[i].used) { slot = i; break; }
            if (slot < 0) { c.stop(); continue; }
            Conn& cn = conns[slot];
            cn.sideA = c;
            cn.sideB.setTimeout(1500);
            if (!cn.sideB.connect(rules[r].host, rules[r].rport)) {
                snprintf(statusMsg, sizeof(statusMsg), "upstream %s:%u refused",
                         rules[r].host, rules[r].rport);
                cn.sideA.stop();
                cn.used = false;
                continue;
            }
            cn.used = true;
            cn.bytesAB = cn.bytesBA = 0;
            connTotal++;
            Features::requestRedraw();
        }
        // relay both directions
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn& cn = conns[i];
            if (!cn.used) continue;
            int avail;
            uint8_t buf[256];
            while ((avail = cn.sideA.available()) > 0) {
                int n = cn.sideA.read(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
                if (n <= 0 || cn.sideB.write(buf, n) != n) { closeConn(cn); break; }
                cn.bytesAB += n;
            }
            if (!cn.used) continue;
            while ((avail = cn.sideB.available()) > 0) {
                int n = cn.sideB.read(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
                if (n <= 0 || cn.sideA.write(buf, n) != n) { closeConn(cn); break; }
                cn.bytesBA += n;
                relayBytes += n;
            }
            if (cn.used && (!cn.sideA.connected() || !cn.sideB.connected())) {
                // give both sides a moment to flush, then drop
                static uint32_t closingAt[MAX_CONNS] = {0};
                if (closingAt[i] == 0) closingAt[i] = millis();
                else if (millis() - closingAt[i] > 300) {
                    closeConn(cn);
                    closingAt[i] = 0;
                }
            }
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TCP BRIDGE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[56];

        if (!bridgeUp) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("cable LAN 192.168.169.0/24 + port", x0 + 10, y0 + 24);
            c.drawString("forwards over the Wi-Fi uplink", x0 + 10, y0 + 36);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString(WifiMgr::isConnected() ? "uplink ok" : "uplink needed: connect Wi-Fi", x0 + 10, y0 + 56);
            if (statusMsg[0]) {
                c.setTextColor(Theme::ERR, Theme::BG);
                c.drawString(statusMsg, x0 + 10, y0 + 76);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: start", x0 + 10, y0 + h - 14);
            return;
        }

        c.setTextColor(Theme::OK, Theme::BG);
        c.drawString("UP  " + WifiMgr::currentSSID(), x0 + 10, y0 + 22);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString("LAN 192.168.169.1 (DHCP on)", x0 + 10, y0 + 36);

        for (int i = 0; i < BridgeCfg::SLOTS; i++) {
            int y = y0 + 56 + i * Theme::ROW_LIST;
            bool sel = (i == selRow);
            c.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
            snprintf(line, sizeof(line), "[%d] %s :%u -> %s:%u %s",
                     i, rules[i].enabled ? "ON " : "off",
                     rules[i].lport, rules[i].host[0] ? rules[i].host : "(empty)",
                     rules[i].rport, sel ? "<" : "");
            c.drawString(line, x0 + 10, y);
        }

        int active = 0;
        for (int i = 0; i < MAX_CONNS; i++) if (conns[i].used) active++;
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        snprintf(line, sizeof(line), "conns %d/%d total %lu  %lu KB relayed",
                 active, MAX_CONNS, (unsigned long)connTotal, (unsigned long)(relayBytes / 1024));
        c.drawString(line, x0 + 10, y0 + h - 30);
        if (statusMsg[0]) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 42);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: rule  click: toggle  hold: stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0 && bridgeUp) {
            selRow = ((selRow + in.rotationSteps) % BridgeCfg::SLOTS + BridgeCfg::SLOTS) % BridgeCfg::SLOTS;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (bridgeUp) {
                if (rules[selRow].host[0]) {
                    rules[selRow].enabled = !rules[selRow].enabled;
                    BridgeCfg::save(selRow, rules[selRow]);
                    applyRules();
                }
            } else {
                startBridge();
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress && bridgeUp) {
            stopBridge();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (BridgeCfg::version() != lastVer) applyRules(); // edited from WebUI
        if (!bridgeUp) return;
        pump();
        static uint32_t lastRedraw = 0;
        if (millis() - lastRedraw > 1000) {
            lastRedraw = millis();
            Features::requestRedraw();
        }
    }

    void onExit() { if (bridgeUp) stopBridge(); }
}

REGISTER_FEATURE("TCP Bridge", "Ethernet", MwIcon::Ethernet, 20,
                 nullptr, input, tick, draw, onExit);
