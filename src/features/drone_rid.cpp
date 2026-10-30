// =====================================================================
//  Wi-Fi/BLE: Drone ID — passive Remote ID receiver (ASTM F3411 /
//  EN 4709). Drones broadcast ID, position, altitude, speed and the
//  operator location. Two transports:
//    - BLE: Service Data UUID 0xFFFA (ASTM), message pack inside
//    - Wi-Fi: beacon vendor IE, OUI FA-0B-BC (Open Drone ID)
//  Rotation: BLE/WIFI source. Click: scan / stop. Passive RX only.
//  Note: drones using Wi-Fi Aware (NAN) are not visible (no NAN on S3).
// =====================================================================
#include "../ui/features.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEScan.h>

namespace {
    enum Src { SRC_BLE, SRC_WIFI };
    int src = SRC_BLE;
    bool listening = false;

    struct Drone {
        bool valid = false;
        char id[24] = "";       // Basic ID or Operator ID
        char selfTxt[24] = "";
        double lat = 0, lon = 0;
        double opLat = 0, opLon = 0;
        float altM = 0, speedMs = 0;
        bool haveLoc = false, haveSys = false, haveSelf = false;
        int rssi = 0;
        uint32_t lastSeen = 0;
    };
    constexpr int MAX_DRONES = 4;
    Drone drones[MAX_DRONES];
    uint32_t packsSeen = 0;
    char statusMsg[48] = "";

    Drone* droneByRssi(int rssi) {
        Drone* freeSlot = nullptr;
        for (int i = 0; i < MAX_DRONES; i++) {
            if (!drones[i].valid) { if (!freeSlot) freeSlot = &drones[i]; continue; }
            if (millis() - drones[i].lastSeen > 30000) { drones[i] = Drone(); if (!freeSlot) freeSlot = &drones[i]; continue; }
        }
        return freeSlot ? freeSlot : &drones[0];
    }

    // One message pack: [counter][msg 25B][msg 25B]...
    void parsePack(const uint8_t* p, int len, int rssi) {
        if (len < 26) return; // counter + at least one 25-byte message
        packsSeen++;
        Drone* d = droneByRssi(rssi);
        d->valid = true;
        d->rssi = rssi;
        d->lastSeen = millis();
        int off = 1;
        while (off + 25 <= len) {
            const uint8_t* m = p + off;
            uint8_t type = m[0] & 0x0F;
            uint8_t proto = m[0] >> 4;
            if (proto <= 1) {
                switch (type) {
                    case 0: { // Basic ID
                        char id[24]; int k = 0;
                        for (int i = 2; i < 25 && k < 23; i++) {
                            if (m[i] >= 0x20 && m[i] < 0x7F) id[k++] = (char)m[i];
                        }
                        id[k] = 0;
                        if (k > 0) strncpy(d->id, id, sizeof(d->id) - 1);
                        break;
                    }
                    case 1: { // Location / Vector
                        int32_t la = (int32_t)((uint32_t)m[8] | ((uint32_t)m[9] << 8) | ((uint32_t)m[10] << 16) | ((uint32_t)m[11] << 24));
                        int32_t lo = (int32_t)((uint32_t)m[12] | ((uint32_t)m[13] << 8) | ((uint32_t)m[14] << 16) | ((uint32_t)m[15] << 24));
                        uint16_t spd = m[4] | (m[5] << 8);
                        uint16_t alt = m[18] | (m[19] << 8);
                        if (la != 0 || lo != 0) {
                            d->lat = la * 1e-7;
                            d->lon = lo * 1e-7;
                            d->speedMs = spd * 0.25f;
                            d->altM = alt * 0.5f;
                            d->haveLoc = true;
                        }
                        break;
                    }
                    case 3: { // Self ID
                        char t[24]; int k = 0;
                        for (int i = 2; i < 25 && k < 23; i++)
                            if (m[i] >= 0x20 && m[i] < 0x7F) t[k++] = (char)m[i];
                        t[k] = 0;
                        if (k > 0) { strncpy(d->selfTxt, t, sizeof(d->selfTxt) - 1); d->haveSelf = true; }
                        break;
                    }
                    case 4: { // System (operator position)
                        int32_t la = (int32_t)((uint32_t)m[8] | ((uint32_t)m[9] << 8) | ((uint32_t)m[10] << 16) | ((uint32_t)m[11] << 24));
                        int32_t lo = (int32_t)((uint32_t)m[12] | ((uint32_t)m[13] << 8) | ((uint32_t)m[14] << 16) | ((uint32_t)m[15] << 24));
                        if (la != 0 || lo != 0) {
                            d->opLat = la * 1e-7;
                            d->opLon = lo * 1e-7;
                            d->haveSys = true;
                        }
                        break;
                    }
                    case 5: { // Operator ID (fallback display id)
                        char id[24]; int k = 0;
                        for (int i = 2; i < 25 && k < 23; i++)
                            if (m[i] >= 0x20 && m[i] < 0x7F) id[k++] = (char)m[i];
                        id[k] = 0;
                        if (k > 0 && !d->id[0]) strncpy(d->id, id, sizeof(d->id) - 1);
                        break;
                    }
                }
            }
            off += 25;
        }
        Features::requestRedraw();
    }

    // ---- Wi-Fi transport: beacon vendor IE OUI FA-0B-BC ----
    void wifiRx(void* buf, wifi_promiscuous_pkt_type_t type) {
        if (type != WIFI_PKT_MGMT) return;
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        const uint8_t* f = pkt->payload;
        int len = pkt->rx_ctrl.sig_len;
        if (len < 40) return;
        if (((f[0] >> 4) & 0x0F) != 8) return; // beacons only
        int pos = 36; // beacon: hdr 24 + 8 timestamp/beacon-interval/capability
        while (pos + 2 <= len - 4) {
            uint8_t id = f[pos], l = f[pos + 1];
            if (id == 0xDD && l >= 10 && f[pos + 2] == 0xFA && f[pos + 3] == 0x0B && f[pos + 4] == 0xBC) {
                parsePack(f + pos + 5, l - 3, pkt->rx_ctrl.rssi);
                return;
            }
            if (id == 0xDD && l == 0xFF) break;
            pos += 2 + l;
        }
    }

    void wifiStart() {
        WiFi.mode(WIFI_STA);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&wifiRx);
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        listening = true;
    }

    // ---- BLE transport: Service Data UUID 0xFFFA ----
    void bleScanOnce() {
        BLEDevice::init("");
        BLEScan* scan = BLEDevice::getScan();
        scan->setActiveScan(false);
        BLEScanResults res = scan->start(2, false);
        int n = res.getCount();
        for (int i = 0; i < n; i++) {
            BLEAdvertisedDevice dev = res.getDevice(i);
            uint8_t* pl = dev.getPayload();
            size_t plLen = dev.getPayloadLength();
            // walk AD structures for type 0x16 with UUID 0xFFFA
            size_t p = 0;
            while (p + 1 < plLen) {
                uint8_t l = pl[p];
                if (l == 0 || p + 1 + l > plLen) break;
                if (pl[p + 1] == 0x16 && l >= 4 && pl[p + 2] == 0xFA && pl[p + 3] == 0xFF) {
                    parsePack(pl + p + 4, l - 3, dev.getRSSI());
                }
                p += 1 + l;
            }
        }
        scan->clearResults();
        snprintf(statusMsg, sizeof(statusMsg), "BLE scan done (%d adv)", n);
        Features::requestRedraw();
    }

    void stopAll() {
        if (listening) { esp_wifi_set_promiscuous(false); listening = false; }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "DRONE REMOTE ID");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[52];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(src == SRC_BLE ? "src: BLE 0xFFFA" : "src: WIFI OUI FA-0B-BC", x0 + 10, y0 + 22);
        snprintf(line, sizeof(line), "packs %lu   passive RX only", (unsigned long)packsSeen);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 34);

        int shown = 0;
        for (int i = 0; i < MAX_DRONES; i++) {
            Drone& d = drones[i];
            if (!d.valid) continue;
            int y = y0 + 50 + shown * 26;
            if (y > y0 + h - 46) break;
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(d.id[0] ? d.id : "(no ID)", x0 + 10, y);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            if (d.haveLoc) {
                snprintf(line, sizeof(line), "%.5f %.5f  %lum %.0fm %dBB",
                         d.lat, d.lon, (unsigned long)((millis() - d.lastSeen) / 1000),
                         d.altM, d.rssi);
            } else {
                snprintf(line, sizeof(line), "no location yet  %dBB", d.rssi);
            }
            c.drawString(line, x0 + 10, y + 8);
            if (d.haveSelf) {
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString(d.selfTxt, x0 + 10, y + 16);
            }
            shown++;
        }
        if (shown == 0) {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString(listening ? "listening..." : "no drones seen yet", x0 + 10, y0 + 54);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 32);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: source   click: scan/stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            src = (src + 1) % 2;
            stopAll();
            statusMsg[0] = 0;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (src == SRC_BLE) {
                stopAll();
                bleScanOnce();
            } else {
                if (listening) stopAll();
                else wifiStart();
            }
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void onExit() { stopAll(); }
}

REGISTER_FEATURE("Drone ID", MwCat::Wifi, MwIcon::Antenna, 65,
                 nullptr, input, nullptr, draw, onExit);
