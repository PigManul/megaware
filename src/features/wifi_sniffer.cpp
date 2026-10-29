// =====================================================================
//  Wi-Fi: Sniffer — пассивный приём 802.11 пакетов через promiscuous
//  режим. Показывает: счётчики по типам, новые MAC-адреса, канал.
//  Клик — старт/стоп. Вращение — смена канала (1-13).
//  Пассивный приём только — ничего не передаёт.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>

namespace {
    bool sniffing = false;
    uint8_t channel = 1;
    uint32_t totalPkts = 0;
    uint32_t beaconPkts = 0;
    uint32_t dataPkts = 0;
    uint32_t lastPktAt = 0;
    uint32_t pktPerSec = 0;
    uint32_t secCounter = 0;
    uint32_t lastSecAt = 0;

    // новые MAC-и
    #define MAX_MACS 20
    uint8_t seenMacs[MAX_MACS][6];
    char macNames[MAX_MACS][18];
    int macCount = 0;

    const char* osDetected = nullptr;

    // IDS: счётчики аномалий
    uint32_t deauthCount = 0;
    uint8_t beaconList[20][6];
    int beaconBssids = 0;

    // QoS: счётчики по Access Categories
    const char* AC_NAMES[4] = {"AC_VO", "AC_VI", "AC_BE", "AC_BK"};
    uint32_t acCount[4] = {0, 0, 0, 0};
    uint32_t qosFrames = 0;

    // TID -> Access Category (802.11e Table 8-3)
    // TID 0,3 -> AC_BE; 1,2 -> AC_BK; 4,5 -> AC_VI; 6,7 -> AC_VO
    int tidToAc(int tid) {
        if (tid >= 6) return 0; // AC_VO
        if (tid >= 4) return 1; // AC_VI
        if (tid >= 2) return 3; // AC_BK
        return 2;               // AC_BE
    }

    // парсить QoS data frame: извлечь TID из QoS Control
    void qosParse(const uint8_t* payload, int len) {
        // 802.11 MAC header для data = 24 bytes (ToDS/FromDS типовые)
        // + QoS Control = 2 bytes (subtype >= 8)
        if (len < 28) return;
        uint8_t fc0 = payload[0];
        uint8_t fc1 = payload[1];
        int type = (fc0 >> 2) & 0x03;
        int subtype = (fc0 >> 4) & 0x0F;
        if (type != 2 || subtype < 8) return; // не QoS data
        bool toDS = (fc1 & 0x01);
        bool fromDS = (fc1 & 0x02);
        int hdrLen = 24;
        if (toDS && fromDS) hdrLen = 30; // 4-address
        if (hdrLen + 2 > len) return;
        uint8_t qosCtl = payload[hdrLen];
        uint8_t qosHi = payload[hdrLen + 1];
        int tid = qosCtl & 0x0F;
        if (tid < 8) return; // TID 0-7 для QoS data
        int ac = tidToAc(tid & 0x07);
        acCount[ac]++;
        qosFrames++;
    }

    // OS fingerprint: DHCP Option 55 сигнатуры
    struct OsSig { const char* os; const uint8_t* opt55; int len; };
    static const uint8_t SIG_WIN[] = {1,3,15,6,112,113,78,79};
    static const uint8_t SIG_LIN[] = {1,3,6,15,28,51,54,58,59};
    static const uint8_t SIG_ANDROID[] = {1,3,6,15,28,51,58,59};
    static const uint8_t SIG_APPLE[] = {1,3,6,15,119,95,252};
    static const OsSig OS_SIGS[] = {
        {"Windows", SIG_WIN, sizeof(SIG_WIN)},
        {"Linux", SIG_LIN, sizeof(SIG_LIN)},
        {"Android", SIG_ANDROID, sizeof(SIG_ANDROID)},
        {"iOS/macOS", SIG_APPLE, sizeof(SIG_APPLE)},
    };

    // парсить DHCP Option 55 из 802.11 data frame
    void dhcpFingerprint(const uint8_t* payload, int len) {
        // ищем LLC/SNAP (AA AA 03) -> IP -> UDP port 67/68 -> DHCP magic
        for (int i = 24; i < len - 20; i++) {
            if (payload[i] == 0xAA && payload[i+1] == 0xAA && payload[i+2] == 0x03) {
                int ipOff = i + 8; // LLC/SNAP = 8 байт
                if (ipOff + 20 > len) return;
                uint8_t proto = payload[ipOff + 9];
                if (proto != 17) return; // UDP
                int udpOff = ipOff + (payload[ipOff] & 0x0F) * 4;
                if (udpOff + 8 > len) return;
                int sport = (payload[udpOff] << 8) | payload[udpOff+1];
                int dport = (payload[udpOff+2] << 8) | payload[udpOff+3];
                if (sport != 67 && sport != 68 && dport != 67 && dport != 68) return;
                int dhcpOff = udpOff + 8;
                if (dhcpOff + 240 > len) return;
                // DHCP magic 0x63825363
                if (payload[dhcpOff+236] != 0x63 || payload[dhcpOff+237] != 0x82 ||
                    payload[dhcpOff+238] != 0x53 || payload[dhcpOff+239] != 0x63) return;
                // опции после magic
                int optOff = dhcpOff + 240;
                while (optOff < len - 2) {
                    uint8_t opt = payload[optOff];
                    if (opt == 0xFF) break;
                    uint8_t olen = payload[optOff+1];
                    if (opt == 55 && olen >= 3) {
                        // сравнить с сигнатурами
                        for (auto& sig : OS_SIGS) {
                            if (olen == sig.len && memcmp(payload+optOff+2, sig.opt55, sig.len) == 0) {
                                osDetected = sig.os;
                                Features::requestRedraw();
                                return;
                            }
                        }
                        osDetected = "unknown";
                        return;
                    }
                    optOff += 2 + olen;
                }
                return;
            }
        }
    }

    void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        totalPkts++;
        if (type == WIFI_PKT_MGMT) beaconPkts++;
        if (type == WIFI_PKT_DATA) dataPkts++;
        // DHCP OS fingerprinting для data-кадров
        if (type == WIFI_PKT_DATA && pkt->rx_ctrl.sig_len > 100) {
            dhcpFingerprint(pkt->payload, pkt->rx_ctrl.sig_len);
            qosParse(pkt->payload, pkt->rx_ctrl.sig_len);
        }
        // IDS: deauth детект (mgmt subtype 12=deauth, 4=probe response от rogue)
        if (type == WIFI_PKT_MGMT && pkt->rx_ctrl.sig_len > 26) {
            const uint8_t* f = pkt->payload;
            int subtype = (f[0] >> 4) & 0x0F;
            if (subtype == 12) { // deauth
                deauthCount++;
                if (deauthCount % 5 == 1) Features::requestRedraw();
            }
            if (subtype == 8) { // beacon — rogue AP детект
                const uint8_t* bssid = f + 16;
                bool known = false;
                for (int i = 0; i < beaconBssids; i++)
                    if (memcmp(beaconList[i], bssid, 6) == 0) { known = true; break; }
                if (!known && beaconBssids < 20) {
                    memcpy(beaconList[beaconBssids], bssid, 6);
                    beaconBssids++;
                }
            }
        }

        // новый MAC?
        const uint8_t* sa = pkt->payload + 10; // addr2 (source)
        bool known = false;
        for (int i = 0; i < macCount; i++) {
            if (memcmp(seenMacs[i], sa, 6) == 0) { known = true; break; }
        }
        if (!known && macCount < MAX_MACS) {
            memcpy(seenMacs[macCount], sa, 6);
            snprintf(macNames[macCount], sizeof(macNames[0]), "%02X:%02X:%02X:%02X:%02X:%02X",
                     sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
            macCount++;
            Features::requestRedraw();
        }
        lastPktAt = millis();

        // packets per second
        if (millis() - lastSecAt >= 1000) {
            pktPerSec = totalPkts - secCounter;
            secCounter = totalPkts;
            lastSecAt = millis();
            Features::requestRedraw();
        }
    }

    void startSniff() {
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&snifferCb);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        memset(acCount, 0, sizeof(acCount));
        qosFrames = 0;
        deauthCount = 0;
        beaconBssids = 0;
        sniffing = true;
    }

    void stopSniff() {
        esp_wifi_set_promiscuous(false);
        sniffing = false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PACKET SNIFFER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!sniffing) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("passive monitor, no transmit", x0 + 10, y0 + 24);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: start (ch 1)", x0 + 10, y0 + 44);
            return;
        }
        char line[48];
        snprintf(line, sizeof(line), "ch %d  %lu pkt/s", channel, (unsigned long)pktPerSec);
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 24);
        snprintf(line, sizeof(line), "total %lu  mgmt %lu  data %lu",
                 (unsigned long)totalPkts, (unsigned long)beaconPkts, (unsigned long)dataPkts);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 42);
        if (osDetected) {
            snprintf(line, sizeof(line), "OS: %s (DHCP)", osDetected);
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 58);
        }
        // QoS по Access Categories
        if (qosFrames > 0) {
            int qy = y0 + (osDetected ? 76 : 58);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString("QoS:", x0 + 10, qy);
            for (int i = 0; i < 4; i++) {
                snprintf(line, sizeof(line), "%s %lu", AC_NAMES[i], (unsigned long)acCount[i]);
                c.setTextColor(i == 0 ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
                c.drawString(line, x0 + 48, qy + 4 + (3 - i) * 14);
            }
        }

        // новые MAC-и (последние 5)
        int show = macCount < 5 ? macCount : 5;
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("devices seen:", x0 + 10, y0 + 66);
        for (int i = 0; i < show; i++) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(macNames[macCount - show + i], x0 + 20, y0 + 82 + i * Theme::ROW_LIST);
        }
        if (deauthCount > 0) {
            char ids[40];
            snprintf(ids, sizeof(ids), "⚠ DEAUTH DETECTED: %lu", (unsigned long)deauthCount);
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(ids, x0 + 10, y0 + h - 28);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: channel  click: stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            if (!sniffing) {
                channel = ((channel - 1 + in.rotationSteps) % 13 + 13) % 13 + 1;
                Features::requestRedraw();
                return true;
            }
            if (sniffing) {
                channel = ((channel - 1 + in.rotationSteps) % 13 + 13) % 13 + 1;
                esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
                Features::requestRedraw();
                return true;
            }
            return false;
        }
        if (in.event == InputEvent::Click) {
            if (sniffing) stopSniff();
            else startSniff();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}

    void onExit() { if (sniffing) stopSniff(); }
}

REGISTER_FEATURE("Sniffer", MwCat::Wifi, MwIcon::Pulse, 50,
                 nullptr, input, tick, draw, onExit);
