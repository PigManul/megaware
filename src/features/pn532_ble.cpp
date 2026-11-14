// =====================================================================
//  NFC: PN532 BLE — talk to an EXTERNAL PN532 module bridged over
//  Bluetooth LE (Nordic UART Service). The bridge device forwards
//  bytes both ways; this feature wraps the PN532 HSU frame protocol:
//    request: 00 00 FF LEN LCS D5 CMD [data..] DCS POST
//    ACK:     00 00 FF 00 FF 00
//  Minimal command set: GetFirmwareVersion + InListPassiveTarget (UID).
//  Target MAC is set from the terminal: pn5ble <mac> (NVS).
//  Click: connect + probe. Rotation: nothing.
// =====================================================================
#include "../ui/features.h"
#include "../hal/config.h"
#include "../net/webui.h"
#include "../../include/theme.h"
#include <BLEDevice.h>

namespace {
    enum State : uint8_t { Idle, Connecting, Ready };
    State state = Idle;
    char statusMsg[44] = "";
    char fwStr[32] = "";
    char uidStr[24] = "";
    uint32_t lastProbe = 0;

    BLEClient* client = nullptr;
    BLERemoteCharacteristic* tx = nullptr; // NUS RX (we write)
    BLERemoteCharacteristic* rx = nullptr; // NUS TX (notify)

    // ---- RX ring buffer for bridge bytes ----
    constexpr size_t RB_CAP = 512;
    uint8_t rbuf[RB_CAP];
    volatile size_t rHead = 0, rTail = 0;

    void rbPush(const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n; i++) {
            size_t next = (rHead + 1) % RB_CAP;
            if (next == rTail) return; // overflow: drop
            rbuf[rHead] = d[i];
            rHead = next;
        }
    }

    void notifyCb(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
        (void)chr;
        rbPush(data, len);
    }

    bool rbReadByte(uint8_t& b, uint32_t timeoutMs) {
        uint32_t until = millis() + timeoutMs;
        while (millis() < until) {
            if (rTail != rHead) { b = rbuf[rTail]; rTail = (rTail + 1) % RB_CAP; return true; }
            delay(1);
        }
        return false;
    }

    void rbFlush() { rTail = rHead; }

    // ---- PN532 HSU frame protocol ----
    bool pn532Cmd(uint8_t cmd, const uint8_t* data, size_t dlen,
                  uint8_t* resp, size_t respCap, size_t& respLen, uint32_t timeoutMs) {
        rbFlush();
        uint8_t fr[64];
        size_t n = 0;
        fr[n++] = 0x00; fr[n++] = 0x00; fr[n++] = 0xFF;
        size_t body = 2 + dlen; // direction + cmd(+data)
        if (body > 250) return false;
        fr[n++] = (uint8_t)body;
        fr[n++] = (uint8_t)(0x100 - body);
        fr[n++] = 0xD4;
        fr[n++] = cmd;
        for (size_t i = 0; i < dlen; i++) fr[n++] = data[i];
        uint8_t dcs = 0;
        for (size_t i = 5; i < n; i++) dcs += fr[i];
        fr[n++] = (uint8_t)(0x100 - dcs);
        fr[n++] = 0x00;
        tx->writeValue(fr, n, false);

        // ACK
        static const uint8_t ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
        uint8_t got[6];
        for (int i = 0; i < 6; i++) {
            if (!rbReadByte(got[i], timeoutMs)) return false;
        }
        if (memcmp(got, ACK, 6) != 0) return false;
        // response frame
        uint8_t h[6];
        for (int i = 0; i < 6; i++) {
            if (!rbReadByte(h[i], timeoutMs)) return false;
        }
        if (h[0] || h[1] || h[2] != 0xFF) return false;
        uint8_t len = h[4];
        if (len + 2 > respCap) return false;
        // data + DCS + POST(0)
        size_t need = len + 2;
        uint8_t tmp[260];
        for (size_t i = 0; i < need; i++) {
            if (!rbReadByte(tmp[i], timeoutMs)) return false;
        }
        if (len < 2) return false;
        memcpy(resp, tmp, len - 2);
        respLen = len - 2;
        return true;
    }

    void connectProbe() {
        String mac = Config::getString("pn5ble_mac", "");
        if (!mac.length()) {
            snprintf(statusMsg, sizeof(statusMsg), "set MAC: pn5ble <mac>");
            return;
        }
        snprintf(statusMsg, sizeof(statusMsg), "connecting...");
        state = Connecting;
        Features::requestRedraw();

        BLEDevice::init("megaware-ble");
        client = BLEDevice::createClient();
        BLEAddress addr(mac.c_str());
        if (!client->connect(addr)) {
            snprintf(statusMsg, sizeof(statusMsg), "connect failed");
            state = Idle;
            return;
        }
        // Nordic UART Service
        static const char* NUS_SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
        BLERemoteService* svc = client->getService(NUS_SVC);
        if (!svc) { snprintf(statusMsg, sizeof(statusMsg), "no NUS service"); state = Idle; return; }
        tx = svc->getCharacteristic(BLEUUID((uint16_t)0x2902).toString().c_str()); // placeholder, replaced below
        // correct characteristics:
        tx = svc->getCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"); // RX (write)
        rx = svc->getCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"); // TX (notify)
        if (!tx || !rx) { snprintf(statusMsg, sizeof(statusMsg), "no NUS chars"); state = Idle; return; }
        rx->registerForNotify(notifyCb);

        // GetFirmwareVersion (cmd 0x02)
        uint8_t resp[32];
        size_t rl = 0;
        if (pn532Cmd(0x02, nullptr, 0, resp, sizeof(resp), rl, 1500) && rl >= 4) {
            snprintf(fwStr, sizeof(fwStr), "PN532 v%d.%d", resp[1], resp[2]);
            // InListPassiveTarget: 106 kbps Type A, 1 target
            uint8_t il[] = {0x01, 0x00};
            uint8_t r2[64];
            size_t r2l = 0;
            if (pn532Cmd(0x4A, il, sizeof(il), r2, sizeof(r2), r2l, 1500) && r2l >= 6 && r2[3] >= 1) {
                // NbTg at r2[3]; response: SENS_RES(2) SEL_RES(1) UID_LEN(1) UID
                uint8_t uidLen = r2[8];
                if (uidLen > 10) uidLen = 10;
                char* p = uidStr;
                for (int i = 0; i < uidLen && p - uidStr + 3 < (int)sizeof(uidStr); i++)
                    p += snprintf(p, 3, "%02X", r2[9 + i]);
                *p = 0;
            } else uidStr[0] = 0;
            snprintf(statusMsg, sizeof(statusMsg), "linked");
            state = Ready;
        } else {
            snprintf(statusMsg, sizeof(statusMsg), "PN532 no response");
            state = Idle;
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PN532 BLE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        String mac = Config::getString("pn5ble_mac", "");
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        snprintf(line, sizeof(line), "bridge MAC: %s", mac.length() ? mac.c_str() : "(unset)");
        c.drawString(line, x0 + 10, y0 + 22);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("NUS bridge (nRF-style PN532), set via", x0 + 10, y0 + 34);
        c.drawString("terminal: pn5ble <mac>", x0 + 10, y0 + 44);

        c.setTextColor(state == Ready ? Theme::OK : Theme::TEXT, Theme::BG);
        if (fwStr[0]) c.drawString(fwStr, x0 + 10, y0 + 62);
        if (uidStr[0]) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            snprintf(line, sizeof(line), "card UID: %s", uidStr);
            c.drawString(line, x0 + 10, y0 + 76);
        }
        if (state == Connecting || statusMsg[0]) {
            c.setTextColor(state == Connecting ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 30);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: connect + probe", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click && state != Connecting) {
            connectProbe();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Ready) return;
        // re-poll for a card every second
        if (millis() - lastProbe > 1000 && tx) {
            lastProbe = millis();
            uint8_t il[] = {0x01, 0x00};
            uint8_t r2[64];
            size_t r2l = 0;
            if (pn532Cmd(0x4A, il, sizeof(il), r2, sizeof(r2), r2l, 400) && r2l >= 6 && r2[3] >= 1) {
                uint8_t uidLen = r2[8];
                if (uidLen > 10) uidLen = 10;
                char* p = uidStr;
                for (int i = 0; i < uidLen && p - uidStr + 3 < (int)sizeof(uidStr); i++)
                    p += snprintf(p, 3, "%02X", r2[9 + i]);
                *p = 0;
                Features::requestRedraw();
            } else if (uidStr[0]) {
                uidStr[0] = 0;
                Features::requestRedraw();
            }
        }
    }

    void onExit() {
        if (client) { client->disconnect(); client = nullptr; }
        BLEDevice::deinit(true);
        state = Idle;
        tx = nullptr;
        rx = nullptr;
        rTail = rHead;
    }
}

REGISTER_FEATURE("PN532 BLE", "NFC", MwIcon::Bluetooth, 60,
                 nullptr, input, tick, draw, onExit);
