// =====================================================================
//  Ethernet: W5500 — wired network through an external module in the
//  Grove port (CS=44, IRQ=43, SPI 9/10/11). Click — bring the interface
//  up (DHCP client). WebUI/API work over the cable afterwards.
//  Bring-up lives in src/hal/eth.cpp (shared with the TCP bridge).
// =====================================================================
#include "../ui/features.h"
#include "../hal/eth.h"
#include "../../include/theme.h"

namespace {
    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "W5500 ETHERNET");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!EthW5500::running()) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("external module in Grove port", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("CS 44  IRQ 43  SPI 9/10/11", x0 + 10, y0 + 42);
            if (EthW5500::lastError()[0]) {
                c.setTextColor(Theme::ERR, Theme::BG);
                c.drawString(EthW5500::lastError(), x0 + 10, y0 + 62);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: start (DHCP)", x0 + 10, y0 + h - 14);
            return;
        }

        bool gotIp = EthW5500::hasIP();
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(EthW5500::netif(), &ip);
        c.setTextColor(gotIp ? Theme::OK : Theme::WARN, Theme::BG);
        c.drawString(gotIp ? "LINK UP (DHCP)" : "started, waiting for DHCP/cable...", x0 + 10, y0 + 26);
        if (gotIp) {
            char line[48];
            snprintf(line, sizeof(line), "IP   %u.%u.%u.%u", (unsigned)(ip.ip.addr & 0xFF),
                     (unsigned)((ip.ip.addr >> 8) & 0xFF), (unsigned)((ip.ip.addr >> 16) & 0xFF),
                     (unsigned)((ip.ip.addr >> 24) & 0xFF));
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 46);
            snprintf(line, sizeof(line), "GW   %u.%u.%u.%u", (unsigned)(ip.gw.addr & 0xFF),
                     (unsigned)((ip.gw.addr >> 8) & 0xFF), (unsigned)((ip.gw.addr >> 16) & 0xFF),
                     (unsigned)((ip.gw.addr >> 24) & 0xFF));
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 62);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("WebUI/API work over cable too", x0 + 10, y0 + h - 14);
        } else if (EthW5500::lastError()[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(EthW5500::lastError(), x0 + 10, y0 + 46);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (EthW5500::running()) EthW5500::stop();
            else EthW5500::start();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        static uint32_t last = 0;
        if (EthW5500::running() && millis() - last > 2000) {
            last = millis();
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("W5500", "Ethernet", MwIcon::Ethernet, 10,
                 nullptr, input, tick, draw, nullptr);
