// =====================================================================
//  Wi-Fi: WPA Monitor — passive EAPOL research on a chosen channel.
//  Passive RX only, nothing is transmitted or injected.
//  Decodes from the air:
//    - RSN IE from beacons: AKM suite (PSK / 802.1X / SAE / OWE...)
//      and PMF (802.11w) capability (required/optional/none)
//    - EAPOL-Key messages M1..M4 per (AP, station) pair - WPA handshake
//      progress detection (M1-M2 pair is the hashcat-usable one)
//    - PMKID from the M1 Key Data (KDE 0xDD/0x14) - exportable to
//      the classic "PMKID*ap*sta*essid*hash" format for offline research
//    - EAP Identity strings from EAP-Packet frames (enterprise 802.1X)
//  Data frames are parsed with QoS Control handling and A-MSDU
//  subframe unwrapping, so EAPOL inside aggregated frames is found.
//  Rotation: channel. Long press: save /wifi/pmkid.hash + /wifi/wpa.log.
// =====================================================================
#include "../ui/features.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

namespace {
    bool running = false;
    uint8_t channel = 1;
    uint32_t eapolSeen = 0;

    // ---- AP table: ssid/akm/pmf per BSSID (from beacons) ----
    struct Ap {
        uint8_t mac[6];
        char ssid[33];
        uint8_t akm;  // 1 802.1X, 2 PSK, 8 SAE, 18 OWE, 0 unknown
        uint8_t pmf;  // 0 none, 1 optional, 2 required
        bool valid;
    };
    constexpr int MAX_APS = 12;
    Ap aps[MAX_APS];

    // ---- handshake pairs: M1..M4 progress + PMKID + EAP identity ----
    struct Hs {
        uint8_t ap[6], sta[6];
        uint8_t m;          // bit0..3 = M1..M4 seen
        bool hasPmkid;
        uint8_t pmkid[16];
        char eapId[33];
        uint32_t last;
        bool valid;
    };
    constexpr int MAX_HS = 10;
    Hs hss[MAX_HS];

    int findAp(const uint8_t* mac) {
        for (int i = 0; i < MAX_APS; i++)
            if (aps[i].valid && memcmp(aps[i].mac, mac, 6) == 0) return i;
        return -1;
    }
    int findHs(const uint8_t* ap, const uint8_t* sta) {
        for (int i = 0; i < MAX_HS; i++)
            if (hss[i].valid && memcmp(hss[i].ap, ap, 6) == 0 && memcmp(hss[i].sta, sta, 6) == 0) return i;
        return -1;
    }

    const char* akmName(uint8_t a) {
        switch (a) {
            case 1: return "802.1X";
            case 2: return "PSK";
            case 3: return "FT-X";
            case 4: return "FT-PSK";
            case 8: return "SAE";
            case 18: return "OWE";
            default: return "?";
        }
    }

    // ---- beacon parse: SSID + RSN IE ----
    void parseBeacon(const uint8_t* f, int len, const uint8_t* rssiMac) {
        (void)rssiMac;
        if (len < 38) return;
        const uint8_t* bssid = f + 16;
        const uint8_t* sa = f + 10;
        // only own beacons (bssid == addr2)
        if (memcmp(sa, bssid, 6) != 0) return;
        int idx = findAp(bssid);
        if (idx < 0) {
            for (int i = 0; i < MAX_APS; i++) {
                if (!aps[i].valid) { idx = i; memset(&aps[i], 0, sizeof(Ap)); aps[i].valid = true;
                                     memcpy(aps[i].mac, bssid, 6); break; }
            }
            if (idx < 0) return;
        }
        Ap& ap = aps[idx];
        int pos = 36;
        while (pos + 2 <= len - 4) {
            uint8_t id = f[pos], l = f[pos + 1];
            if (pos + 2 + l > len) break;
            if (id == 0 && !ap.ssid[0] && l > 0 && l <= 32) {
                memcpy(ap.ssid, f + pos + 2, l);
                ap.ssid[l] = 0;
                for (int i = 0; i < l; i++) if ((uint8_t)ap.ssid[i] < 0x20) ap.ssid[i] = '.';
            } else if (id == 0x30 && l >= 8) {
                // RSN: ver(2) group(4) pcnt(2) suites pcnt*4 acnt(2) suites acnt*4 cap(2)
                int o = pos + 2;
                int pcnt = (f[o + 4] << 8) | f[o + 5];
                int aoff = o + 6 + pcnt * 4;
                int acnt = 0;
                if (aoff + 2 <= pos + 2 + l) {
                    acnt = (f[aoff] << 8) | f[aoff + 1];
                    if (acnt >= 1 && aoff + 2 + 4 <= pos + 2 + l) ap.akm = f[aoff + 2 + 3];
                }
                int coff = aoff + 2 + acnt * 4;
                if (coff + 2 <= pos + 2 + l) {
                    uint16_t cap = (f[coff] << 8) | f[coff + 1];
                    bool mfpr = cap & 0x40, mfpc = cap & 0x80;
                    ap.pmf = mfpr ? 2 : (mfpc ? 1 : 0);
                }
            }
            pos += 2 + l;
            if (id == 0xDD && l == 0xFF) break;
        }
    }

    // ---- EAPOL handling ----
    void parseEapol(const uint8_t* e, int len, const uint8_t* ap, const uint8_t* sta) {
        if (len < 6) return;
        eapolSeen++;
        int hi = findHs(ap, sta);
        if (hi < 0) {
            for (int i = 0; i < MAX_HS; i++) {
                if (!hss[i].valid) {
                    hi = i;
                    memset(&hss[i], 0, sizeof(Hs));
                    hss[i].valid = true;
                    memcpy(hss[i].ap, ap, 6);
                    memcpy(hss[i].sta, sta, 6);
                    break;
                }
            }
            if (hi < 0) return;
        }
        Hs& h = hss[hi];
        h.last = millis();
        uint8_t type = e[1];

        if (type == 0 && len >= 10) { // EAP-Packet: identity extraction
            uint8_t code = e[4];
            int eapLen = (e[6] << 8) | e[7];
            if ((code == 1 || code == 2) && eapLen >= 6 && e[8] == 1 && eapLen <= len - 4) {
                int ilen = eapLen - 5;
                if (ilen > 32) ilen = 32;
                if (ilen > 0 && !h.eapId[0]) {
                    memcpy(h.eapId, e + 9, ilen);
                    h.eapId[ilen] = 0;
                    Features::requestRedraw();
                }
            }
            return;
        }
        if (type != 3 || len < 99) return; // EAPOL-Key with full descriptor

        uint16_t ki = ((uint16_t)e[5] << 8) | e[6];
        bool pairwise = ki & 0x0008;
        bool ack = ki & 0x0080;
        bool mic = ki & 0x0100;
        bool secure = ki & 0x0200;
        if (!pairwise) return;

        if (ack && !mic) h.m |= 0x01;                        // M1
        else if (!ack && mic && !secure) h.m |= 0x02;        // M2
        else if (ack && mic && secure) h.m |= 0x04;          // M3
        else if (!ack && mic && secure) h.m |= 0x08;         // M4

        // PMKID: first KDE in M1 key data: 0xDD 0x14 <16 bytes>
        if ((h.m & 0x01) && !h.hasPmkid && len >= 97 + 18) {
            int kdLen = (e[95] << 8) | e[96];
            if (kdLen >= 18 && e[97] == 0xDD && e[98] == 0x14) {
                memcpy(h.pmkid, e + 99, 16);
                h.hasPmkid = true;
            }
        }
        if (h.m) Features::requestRedraw();
    }

    // ---- data frame: QoS Control + A-MSDU unwrap + LLC/SNAP match ----
    void parseData(const uint8_t* f, int len) {
        if (len < 32) return;
        uint8_t fc0 = f[0], fc1 = f[1];
        int subtype = (fc0 >> 4) & 0x0F;
        bool toDS = fc1 & 0x01, fromDS = fc1 & 0x02;
        int hdrLen = 24;
        if (toDS && fromDS) hdrLen = 30;
        bool qos = (subtype >= 8);
        int dataOff = hdrLen + (qos ? 2 : 0);
        bool amsdu = false;
        if (qos && dataOff <= len) amsdu = f[hdrLen] & 0x80;

        static const uint8_t LLC_EAPOL[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        const uint8_t* ap = toDS ? f + 4 : f + 10;
        const uint8_t* sta = toDS ? f + 10 : f + 4;

        auto checkEapol = [&](const uint8_t* p, int plen) {
            if (plen < 8 + 4 || memcmp(p, LLC_EAPOL, 8) != 0) return;
            parseEapol(p + 8, plen - 8, ap, sta);
        };

        if (amsdu) {
            // subframes: [DA 6][SA 6][len 2 BE][body pad to 4]
            int o = dataOff;
            while (o + 16 <= len) {
                int sub = (f[o + 12] << 8) | f[o + 13];
                if (sub < 8 || o + 14 + sub > len) break;
                checkEapol(f + o + 14, sub);
                o += 14 + sub + ((4 - (sub % 4)) % 4);
            }
        } else {
            checkEapol(f + dataOff, len - dataOff);
        }
    }

    void rx(void* buf, wifi_promiscuous_pkt_type_t type) {
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        const uint8_t* f = pkt->payload;
        int len = pkt->rx_ctrl.sig_len;
        if (type == WIFI_PKT_MGMT && len > 26 && (((f[0] >> 4) & 0x0F) == 8)) parseBeacon(f, len, nullptr);
        if (type == WIFI_PKT_DATA) parseData(f, len);
    }

    void startMon() {
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&rx);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        running = true;
    }
    void stopMon() { esp_wifi_set_promiscuous(false); running = false; }

    void saveAll() {
        if (!SD.exists("/wifi")) SD.mkdir("/wifi");
        File h = SD.open("/wifi/pmkid.hash", FILE_APPEND);
        if (h) {
            for (int i = 0; i < MAX_HS; i++) {
                if (!hss[i].valid || !hss[i].hasPmkid) continue;
                int ai = findAp(hss[i].ap);
                const char* ssid = (ai >= 0 && aps[ai].ssid[0]) ? aps[ai].ssid : "(unknown)";
                char hex[33];
                for (int b = 0; b < 16; b++) snprintf(hex + b * 2, 3, "%02X", hss[i].pmkid[b]);
                h.printf("PMKID*%02X%02X%02X%02X%02X%02X*%02X%02X%02X%02X%02X%02X*%s*%s\n",
                         hss[i].ap[0], hss[i].ap[1], hss[i].ap[2], hss[i].ap[3], hss[i].ap[4], hss[i].ap[5],
                         hss[i].sta[0], hss[i].sta[1], hss[i].sta[2], hss[i].sta[3], hss[i].sta[4], hss[i].sta[5],
                         ssid, hex);
            }
            h.close();
        }
        File l = SD.open("/wifi/wpa.log", FILE_APPEND);
        if (l) {
            for (int i = 0; i < MAX_HS; i++) {
                if (!hss[i].valid) continue;
                int ai = findAp(hss[i].ap);
                const char* ssid = (ai >= 0 && aps[ai].ssid[0]) ? aps[ai].ssid : "?";
                l.printf("%02X%02X%02X%02X%02X%02X ssid=%s M=%u%u%u%u pmkid=%s eap=%s\n",
                         hss[i].ap[0], hss[i].ap[1], hss[i].ap[2], hss[i].ap[3], hss[i].ap[4], hss[i].ap[5],
                         ssid, (hss[i].m >> 0) & 1, (hss[i].m >> 1) & 1, (hss[i].m >> 2) & 1, (hss[i].m >> 3) & 1,
                         hss[i].hasPmkid ? "yes" : "no", hss[i].eapId[0] ? hss[i].eapId : "-");
            }
            l.close();
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "WPA MONITOR");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[56];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "ch %d  eapol %lu  passive RX", channel, (unsigned long)eapolSeen);
        c.drawString(line, x0 + 10, y0 + 20);

        int shown = 0;
        for (int i = 0; i < MAX_HS && shown < 4; i++) {
            if (!hss[i].valid) continue;
            if (millis() - hss[i].last > 120000) continue;
            int ai = findAp(hss[i].ap);
            const char* ssid = (ai >= 0 && aps[ai].ssid[0]) ? aps[ai].ssid : "?";
            const char* akm = ai >= 0 ? akmName(aps[ai].akm) : "?";
            int y = y0 + 38 + shown * 30;
            c.setTextColor(Theme::ACCENT, Theme::BG);
            snprintf(line, sizeof(line), "%.20s %s%s", ssid, akm,
                     (ai >= 0 && aps[ai].pmf == 2) ? " PMF!" : ((ai >= 0 && aps[ai].pmf == 1) ? " pmf" : ""));
            c.drawString(line, x0 + 10, y);
            c.setTextColor(Theme::TEXT, Theme::BG);
            snprintf(line, sizeof(line), "M[%s%s%s%s] %s %s",
                     (hss[i].m & 1) ? "1" : "-", (hss[i].m & 2) ? "2" : "-",
                     (hss[i].m & 4) ? "3" : "-", (hss[i].m & 8) ? "4" : "-",
                     hss[i].hasPmkid ? "PMKID!" : "     ",
                     hss[i].eapId);
            c.drawString(line, x0 + 10, y + 10);
            shown++;
        }
        // AP summary line
        int apCount = 0, pmfCount = 0;
        for (int i = 0; i < MAX_APS; i++)
            if (aps[i].valid) { apCount++; if (aps[i].pmf) pmfCount++; }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        snprintf(line, sizeof(line), "APs seen: %d (pmf: %d)", apCount, pmfCount);
        c.drawString(line, x0 + 10, y0 + h - 26);

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(running ? "rot: channel  hold: save hash/log" : "click: start", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            channel = ((channel - 1 + in.rotationSteps) % 13 + 13) % 13 + 1;
            if (running) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (running) stopMon();
            else startMon();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            saveAll();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void onExit() { if (running) stopMon(); }
}

REGISTER_FEATURE("WPA Monitor", MwCat::Wifi, MwIcon::Lock, 70,
                 nullptr, input, nullptr, draw, onExit);
