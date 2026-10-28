// =====================================================================
//  Wi-Fi: AP Spam — поток фиктивных beacon-кадров: список точек доступа
//  без пароля, которые ничего не делают (клиентов не принимают).
//  Источники имён: /spam/ssids.txt с SD/Flash (по одному SSID в строке)
//  либо случайные "xyxyx" (5 символов, согласная-гласная-...).
//  Клик — старт/стоп. Вращение — источник (Random / File).
//  Кадры шлются через esp_wifi_80211_tx с канала на канал (1..13).
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum Mode : uint8_t { RandomNames, FromFile };
    Mode mode = RandomNames;
    bool spamming = false;
    uint32_t sent = 0;

    // список из файла: до 32 SSID по 32 символа
    char listSsids[32][33];
    int listCount_ = 0;
    int listIdx = 0;

    fs::FS& spamFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    uint32_t rngState = 0x1234abcd;
    uint8_t rnd() { rngState = rngState * 1664525u + 1013904223u; return (uint8_t)(rngState >> 24); }

    void genRandomSsid(char out[6]) {
        const char* cons = "bcdfghjklmnprstvwz";
        const char* vow = "aeiou";
        // формат xyxyx: согласная-гласная-согласная-гласная-согласная
        out[0] = cons[rnd() % 18];
        out[1] = vow[rnd() % 5];
        out[2] = cons[rnd() % 18];
        out[3] = vow[rnd() % 5];
        out[4] = cons[rnd() % 18];
        out[5] = 0;
    }

    void loadList() {
        listCount_ = 0;
        File f = spamFs().open("/spam/ssids.txt", FILE_READ);
        if (!f) return;
        while (f.available() && listCount_ < 32) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0 || line.length() > 32 || line.startsWith("#")) continue;
            snprintf(listSsids[listCount_], sizeof(listSsids[0]), "%s", line.c_str());
            listCount_++;
        }
        f.close();
    }

    // beacon: FC(2) dur(2) DA(6) SA(6) BSS(6) seq(2) | ts(8) int(2) cap(2) | IEs
    void sendBeacon(const char* ssid, uint8_t ch) {
        uint8_t pkt[80];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80; pkt[1] = 0x00;                 // beacon, no flags
        memset(pkt + 4, 0xFF, 6);                     // DA: broadcast
        for (int i = 0; i < 6; i++) {                 // SA/BSS: случайный MAC (local, unicast)
            uint8_t r = rnd();
            pkt[10 + i] = i == 0 ? (r | 0x02) & 0xFE : r;
            pkt[16 + i] = pkt[10 + i];
        }
        // [24..31] timestamp = 0; [32..33] interval = 100 TU
        pkt[32] = 0x64; pkt[33] = 0x00;
        pkt[34] = 0x01; pkt[35] = 0x00;               // capability: ESS (open)
        int p = 36;
        size_t sl = strlen(ssid);
        if (sl > 32) sl = 32;
        pkt[p++] = 0x00;                              // SSID tag
        pkt[p++] = (uint8_t)sl;
        memcpy(pkt + p, ssid, sl); p += sl;
        pkt[p++] = 0x01; pkt[p++] = 0x08;             // supported rates
        pkt[p++] = 0x82; pkt[p++] = 0x84; pkt[p++] = 0x8B; pkt[p++] = 0x96;
        pkt[p++] = 0x24; pkt[p++] = 0x30; pkt[p++] = 0x48; pkt[p++] = 0x6C;
        pkt[p++] = 0x03; pkt[p++] = 0x01; pkt[p++] = ch; // DS param: channel
        esp_wifi_80211_tx(WIFI_IF_AP, pkt, p, false);
    }

    void spamBurst(int count) {
        for (int i = 0; i < count; i++) {
            char ssid[33];
            if (mode == FromFile && listCount_ > 0) {
                snprintf(ssid, sizeof(ssid), "%s", listSsids[listIdx % listCount_]);
                listIdx++;
            } else {
                genRandomSsid(ssid);
            }
            uint8_t ch = 1 + (rnd() % 13);
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            sendBeacon(ssid, ch);
            sent++;
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "AP SPAM");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("fake open APs, no clients served", x0 + 10, y0 + 24);
        const char* modeName = mode == RandomNames ? "random xyxyx" : "/spam/ssids.txt";
        char line[56];
        snprintf(line, sizeof(line), "source: %s%s", modeName,
                 mode == FromFile ? (listCount_ ? "" : " (no file!)") : "");
        c.setTextColor(mode == FromFile && !listCount_ ? Theme::WARN : Theme::TEXT, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 42);
        snprintf(line, sizeof(line), "beacons sent: %lu", (unsigned long)sent);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(line, x0 + 10, y0 + 58);
        c.setTextColor(spamming ? Theme::ACCENT : Theme::TEXT_FAINT, Theme::BG);
        c.drawString(spamming ? "SPAMMING..." : "idle", x0 + 10, y0 + 74);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: start/stop   rotate: source", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            mode = (mode == RandomNames) ? FromFile : RandomNames;
            if (mode == FromFile) loadList();
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (spamming) {
                spamming = false;
            } else {
                if (mode == FromFile) loadList();
                if (mode == FromFile && listCount_ == 0) {
                    Features::requestRedraw();
                    return true; // нечего спамить
                }
                // RAW-инжекция требует запущенного Wi-Fi (AP-интерфейс)
                if (!WifiMgr::isAPRunning() && !WifiMgr::isConnected()) WifiMgr::startAP(0);
                esp_wifi_set_promiscuous(true);
                spamming = true;
                sent = 0;
            }
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (!spamming) return;
        spamBurst(20); // 20 фейковых точек за тик
        static uint32_t lastUi = 0;
        if (millis() - lastUi > 300) { lastUi = millis(); Features::requestRedraw(); }
    }

    void onExit() {
        spamming = false;
        esp_wifi_set_promiscuous(false);
    }
}

REGISTER_FEATURE("AP Spam", MwCat::Wifi, MwIcon::Others, 50,
                 nullptr, input, tick, draw, onExit);
