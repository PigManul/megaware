// =====================================================================
//  Wi-Fi: Probe Log — harvests SSIDs leaked in probe requests.
//  Unassociated phones broadcast the saved network names in directed
//  probes: MAC -> OUI vendor -> RSSI -> SSID list over time. Classic
//  Wi-Fi research (device presence + home-network disclosure).
//  Click: start/stop. Long press: append the table to /wifi/probes.log.
//  Passive RX only.
// =====================================================================
#include "../ui/features.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

namespace {
    bool running = false;
    uint32_t probeCount = 0;
    uint32_t uniqMacs = 0;

    struct SsidHit {
        char ssid[33];
        char mac[18];
        char vendor[10];
        int8_t rssi;
        uint32_t hits;
    };
    constexpr int MAX_HITS = 20;
    SsidHit hits[MAX_HITS];
    int hitCount = 0;

    const char* ouiVendor(const uint8_t* m) {
        // small table of common phone/laptop OUI prefixes
        static const struct { uint8_t b0, b1, b2; const char* name; } T[] = {
            {0xF0,0x18,0x98,"Apple"},{0xAC,0xBC,0x32,"Apple"},{0xD0,0x03,0x4B,"Apple"},
            {0x8C,0x85,0x90,"Apple"},{0xA4,0x83,0xE7,"Apple"},{0x00,0x17,0xF2,"Apple"},
            {0x3C,0x5A,0xB4,"Google"},{0x00,0x1A,0x11,"Google"},{0xF4,0xF5,0xD8,"Google"},
            {0x8C,0x77,0x12,"Google"},{0xDC,0xD4,0x1C,"Xiaomi"},{0x64,0x09,0x80,"Xiaomi"},
            {0x18,0x59,0x36,"Xiaomi"},{0x78,0x02,0xF8,"Xiaomi"},{0x38,0xA2,0x8C,"Huawei"},
            {0x00,0x46,0x4B,"Huawei"},{0x34,0x6B,0xD3,"Huawei"},{0x54,0x60,0x09,"Huawei"},
        };
        for (auto& t : T)
            if (t.b2 && m[0] == t.b0 && m[1] == t.b1 && m[2] == t.b2) return t.name;
        return "";
    }

    int findHit(const char* ssid, const char* mac) {
        for (int i = 0; i < hitCount; i++)
            if (strcmp(hits[i].ssid, ssid) == 0 && strcmp(hits[i].mac, mac) == 0) return i;
        return -1;
    }

    void rx(void* buf, wifi_promiscuous_pkt_type_t type) {
        if (type != WIFI_PKT_MGMT) return;
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        const uint8_t* f = pkt->payload;
        int len = pkt->rx_ctrl.sig_len;
        if (len < 26) return;
        int subtype = (f[0] >> 4) & 0x0F;
        if (subtype != 4) return; // probe request
        probeCount++;
        const uint8_t* sa = f + 10;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        const char* vendor = ouiVendor(sa);

        // walk tagged parameters for SSID (id 0)
        int pos = 24;
        while (pos + 2 <= len - 4) {
            uint8_t id = f[pos], l = f[pos + 1];
            if (id == 0) {
                if (l == 0 || l > 32) break; // wildcard or too long
                char ssid[33];
                memcpy(ssid, f + pos + 2, l);
                ssid[l] = 0;
                for (int i = 0; i < l; i++)
                    if ((uint8_t)ssid[i] < 0x20) ssid[i] = '.';
                int idx = findHit(ssid, mac);
                if (idx < 0 && hitCount < MAX_HITS) {
                    idx = hitCount++;
                    strncpy(hits[idx].ssid, ssid, sizeof(hits[idx].ssid) - 1);
                    hits[idx].ssid[sizeof(hits[idx].ssid) - 1] = 0;
                    strncpy(hits[idx].mac, mac, sizeof(hits[idx].mac));
                    strncpy(hits[idx].vendor, vendor, sizeof(hits[idx].vendor) - 1);
                    hits[idx].hits = 0;
                    Features::requestRedraw();
                }
                if (idx >= 0) {
                    hits[idx].rssi = pkt->rx_ctrl.rssi;
                    hits[idx].hits++;
                }
                break;
            }
            pos += 2 + l;
            if (id == 221 && l == 0xFF) break; // malformed guard
        }
        if (probeCount % 20 == 1) Features::requestRedraw();
    }

    void startLog() {
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&rx);
        // hop channels slowly: probes are heard on the client's channel
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        running = true;
    }

    void stopLog() { esp_wifi_set_promiscuous(false); running = false; }

    void saveLog() {
        if (!SD.exists("/wifi")) SD.mkdir("/wifi");
        File f = SD.open("/wifi/probes.log", FILE_APPEND);
        if (!f) return;
        for (int i = 0; i < hitCount; i++) {
            f.printf("%s,%s,%s,%d,%lu\n", hits[i].ssid, hits[i].mac,
                     hits[i].vendor[0] ? hits[i].vendor : "-", hits[i].rssi,
                     (unsigned long)hits[i].hits);
        }
        f.close();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PROBE LOGGER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        char line[52];
        c.setTextColor(running ? Theme::OK : Theme::TEXT_DIM, Theme::BG);
        c.drawString(running ? "LISTENING (ch hop)" : "idle", x0 + 10, y0 + 22);
        snprintf(line, sizeof(line), "probes %lu   unique SSID+MAC %d", (unsigned long)probeCount, hitCount);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 40);

        int show = hitCount;
        for (int i = 0; i < show; i++) {
            int y = y0 + 60 + i * Theme::ROW_LIST;
            if (y > y0 + h - 18) { c.setTextColor(Theme::TEXT_FAINT, Theme::BG); c.drawString("...", x0 + 10, y); break; }
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(hits[i].ssid, x0 + 10, y);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            snprintf(line, sizeof(line), "%s %s %ddB x%lu",
                     hits[i].vendor[0] ? hits[i].vendor : "   ", hits[i].mac,
                     hits[i].rssi, (unsigned long)hits[i].hits);
            c.drawString(line, x0 + 10, y + 7);
        }
        if (hitCount == 0 && running) {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("waiting for directed probes...", x0 + 10, y0 + 60);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(running ? "click: stop  hold: save CSV" : "click: start", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (running) stopLog();
            else startLog();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            saveLog();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void onExit() { if (running) stopLog(); }
}

REGISTER_FEATURE("Probe Log", MwCat::Wifi, MwIcon::Wifi, 60,
                 nullptr, input, nullptr, draw, onExit);
