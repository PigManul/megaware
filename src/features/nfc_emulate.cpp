// =====================================================================
//  NFC: Emulate — устройство притворяется картой.
//  Два режима:
//   1) UID — притворяемся картой с UID из дампа (работает ТОЛЬКО на
//      ридерах, проверяющих UID без CRYPTO1-auth; MIFARE data-emulation
//      невозможна: ISO14443-3A FDT ~86 мкс, а шина I2C + ESP32 медленнее)
//   2) NDEF — T4T-эмуляция: отвечаем на SELECT NDEF-приложения
//      (AID D2760000850101), CC и READ BINARY — телефон видит NDEF-метку
//      (экспериментально: зависит от лояльности ридера к таймингам)
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { Select, Emulating };
    State state = Select;
    int emuMode = 0;            // 0 = UID dumps, 1 = NDEF files
    int selIndex = 0;
    char openName[64] = "";

    void baseName(char* out, size_t n) {
        snprintf(out, n, "%s", openName);
        char* dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }
    uint8_t emuUid[10] = {0};
    uint8_t emuUidLen = 0;
    uint8_t emuSak = 0x08;    // MIFARE Classic 1K default SAK
    uint32_t emuHits = 0;
    uint32_t pollFailures = 0;
    char emuName[48] = "";
    char statusMsg[52] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    // ---- T4T NDEF emulation state ----
    bool emuNdef = false;
    uint8_t ndefMsg[512];
    uint16_t ndefLen_ = 0;
    // selected file inside the emulated tag: 0 = none, 1 = CC, 2 = NDEF
    uint8_t t4tFile = 0;
    uint8_t t4tCc[15] = {0x00, 0x0F, 0x20, 0x00, 0x3B, 0x00, 0x34,
                         0x04, 0x06, 0xE1, 0x04, 0x00, 0x00, 0x00, 0x00};

    fs::FS& emuFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    // Извлечь UID/SAK из первых байтов .bin дампа Classic (block 0)
    bool extractUid(const uint8_t* buf, uint32_t size, uint8_t* uid, uint8_t& uidLen, uint8_t& sak) {
        if (size < 16) return false;
        // BCC (byte 4) = UID[0] ^ UID[1] ^ UID[2] ^ UID[3] для 4-byte UID
        uint8_t bcc = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
        if (buf[4] != bcc) {
            // не 4-byte UID — попробовать 7-byte (BCC на байте 7)
            uint8_t bcc7 = buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5] ^ buf[6];
            if (buf[7] == bcc7) { uidLen = 7; }
            else { uidLen = 4; } // fallback
        } else {
            uidLen = 4;
        }
        memcpy(uid, buf, uidLen);
        // SAK: для Classic 1K = 0x08, 4K = 0x18
        sak = (size >= 4096) ? 0x18 : 0x08;
        return uidLen > 0;
    }

    void startEmulate() {
        char path[96];
        char base[48]; baseName(base, sizeof(base));

        if (emuMode == 1) {
            // ---- NDEF T4T: build the message from an /ndef file ----
            snprintf(path, sizeof(path), "/ndef/%s", openName);
            File f = emuFs().open(path, FILE_READ);
            if (!f) { setStatus("ndef file not found", false); return; }
            String line1 = f.readStringUntil('\n'); line1.trim();
            String rest = f.readStringUntil(0);
            f.close();
            // same record semantics as the Write feature
            String payload;
            const char* kind = "URL";
            if (line1.startsWith("URL:")) payload = line1.substring(4);
            else if (line1.startsWith("TEXT:")) { kind = "TEXT"; payload = line1.substring(5); }
            else if (line1.startsWith("VCARD:")) {
                kind = "VCARD";
                rest.trim();
                payload = rest;
            } else payload = line1;
            if (payload.length() > 400) payload = payload.substring(0, 400);

            size_t n = 0;
            if (strcmp(kind, "URL") == 0) {
                size_t pl = 1 + payload.length();
                ndefMsg[n++] = 0xD1; ndefMsg[n++] = 0x01; ndefMsg[n++] = (uint8_t)pl;
                ndefMsg[n++] = 0x55; ndefMsg[n++] = 0x00;
            } else if (strcmp(kind, "TEXT") == 0) {
                size_t pl = 3 + payload.length();
                ndefMsg[n++] = 0xD1; ndefMsg[n++] = 0x01; ndefMsg[n++] = (uint8_t)pl;
                ndefMsg[n++] = 0x54; ndefMsg[n++] = 0x02;
                ndefMsg[n++] = 'e'; ndefMsg[n++] = 'n';
            } else {
                static const char* MT = "text/vcard";
                size_t pl = strlen(MT) + payload.length();
                ndefMsg[n++] = 0xD2; ndefMsg[n++] = 0x01; ndefMsg[n++] = (uint8_t)pl;
                memcpy(ndefMsg + n, MT, strlen(MT)); n += strlen(MT);
            }
            memcpy(ndefMsg + n, payload.c_str(), payload.length());
            n += payload.length();
            ndefLen_ = (uint16_t)n;

            // CC with the NDEF file size (NDEF len + 2 length bytes)
            uint16_t fsz = ndefLen_ + 2;
            t4tCc[11] = (uint8_t)(fsz >> 8);
            t4tCc[12] = (uint8_t)(fsz & 0xFF);

            // random UID, Type 4 SAK
            for (int i = 0; i < 4; i++) emuUid[i] = esp_random() & 0xFF;
            emuUidLen = 4;
            emuSak = 0x20;
            emuNdef = true;
            t4tFile = 0;
            if (!Nfc::emulateStart(emuUid, emuUidLen, emuSak)) {
                setStatus("emulate start failed", false);
                return;
            }
            emuHits = 0;
            pollFailures = 0;
            state = Emulating;
            return;
        }

        // ---- UID mode: from a Classic dump ----
        snprintf(path, sizeof(path), "/rfid/%s.bin", base);
        emuNdef = false;
        File f = emuFs().open(path, FILE_READ);
        if (!f) { setStatus("dump not found", false); return; }
        uint8_t block0[16];
        f.read(block0, 16);
        f.close();

        if (!extractUid(block0, 16, emuUid, emuUidLen, emuSak)) {
            setStatus("invalid dump (no UID)", false);
            return;
        }
        if (!Nfc::emulateStart(emuUid, emuUidLen, emuSak)) {
            setStatus("emulate start failed", false);
            return;
        }
        emuHits = 0;
        pollFailures = 0;
        state = Emulating;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "EMULATE");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Select) {
            const char* dir = emuMode == 0 ? "/rfid" : "/ndef";
            FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
            int n = Files::listCount(root, dir);
            int visible = (h - 52) / Theme::ROW_LIST;
            int selC = n ? (selIndex % n + n) % n : 0;
            int top = selC - visible + 1;
            if (top < 0) top = 0;
            c.setTextDatum(textdatum_t::middle_left);
            // mode header
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(emuMode == 0 ? "[UID]  NDEF" : " UID  [NDEF]", x0 + 10, y0 + 20);
            for (int i = 0; i < visible && top + i < n; i++) {
                int idx = top + i;
                FsEntry e = Files::listEntry(root, dir, idx);
                bool s = (idx == selC);
                int yy = y0 + 34 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
                if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, yy); }
                c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
                c.drawString(e.name, x0 + 12, yy);
            }
            if (n == 0) {
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString(emuMode == 0 ? "no dumps — use Dump first" : "no ndef files — upload /ndef/*.txt",
                             x0 + 12, y0 + 34);
            }
            c.setTextDatum(textdatum_t::top_left);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("rot: switch list / select   click: emulate", x0 + 10, y0 + h - 14);
            return;
        }

        // Emulating state
        if (emuHits > 0) {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString("READER DETECTED", x0 + 10, y0 + 26);
        } else {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(emuNdef ? "NDEF T4T EMULATING..." : "EMULATING...", x0 + 10, y0 + 26);
        }

        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);
        char uidBuf[28];
        int pos = 0;
        for (int i = 0; i < emuUidLen && pos + 2 < (int)sizeof(uidBuf); i++)
            pos += snprintf(uidBuf + pos, sizeof(uidBuf) - pos, "%02X", emuUid[i]);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(uidBuf, x0 + 10, y0 + 48);
        snprintf(uidBuf, sizeof(uidBuf), "%d-byte UID  SAK 0x%02X", emuUidLen, emuSak);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(uidBuf, x0 + 10, y0 + 64);
        snprintf(uidBuf, sizeof(uidBuf), "reader hits: %lu", (unsigned long)emuHits);
        c.drawString(uidBuf, x0 + 10, y0 + 80);

        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + 100);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("UID-only (no MIFARE auth)", x0 + 10, y0 + h - 28);
        c.drawString("click / Quit: stop", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;

        if (state == Select) {
            const char* dir = emuMode == 0 ? "/rfid" : "/ndef";
            FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
            int n = Files::listCount(root, dir);
            if (in.rotationSteps != 0) {
                if (n > 0) {
                    selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
                } else {
                    emuMode = (emuMode + 1) % 2; // empty list — rot flips mode
                    selIndex = 0;
                }
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, dir, selC);
                snprintf(openName, sizeof(openName), "%s", e.name);
                startEmulate();
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress && n > 0) {
                emuMode = (emuMode + 1) % 2;
                selIndex = 0;
                Features::requestRedraw();
                return true;
            }
            return false;
        }

        // Emulating
        if (in.event == InputEvent::Click || in.event == InputEvent::UserKey) {
            Nfc::emulateStop();
            state = Select;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Emulating) return;

        static uint32_t lastPoll = 0;
        if (millis() - lastPoll < 50) return; // не чаще 20 Гц
        lastPoll = millis();

        uint8_t data[64];
        uint8_t dataLen = sizeof(data);
        if (Nfc::emulatePoll(data, &dataLen, 30)) {
            emuHits++;
            pollFailures = 0;

            if (emuNdef && dataLen >= 5 && data[0] == 0x00) {
                // ---- ISO14443-4 / T4T APDU handling ----
                uint8_t resp[40];
                size_t rl = 0;
                bool handled = true;
                if (data[1] == 0xA4 && data[2] == 0x04) {
                    // SELECT by AID: our NDEF app D276 00 00 85 01 01?
                    static const uint8_t NDEF_AID[7] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};
                    if (dataLen >= 15 && memcmp(data + 8, NDEF_AID, 7) == 0) {
                        resp[rl++] = 0x90; resp[rl++] = 0x00;
                    } else { resp[rl++] = 0x6A; resp[rl++] = 0x82; }
                } else if (data[1] == 0xA4 && data[3] == 0x0C) {
                    // SELECT by file id (E1 03 = CC, E1 04 = NDEF)
                    if (dataLen >= 9 && data[7] == 0xE1) {
                        t4tFile = data[8] == 0x03 ? 1 : (data[8] == 0x04 ? 2 : 0);
                        resp[rl++] = 0x90; resp[rl++] = 0x00;
                    } else { resp[rl++] = 0x6A; resp[rl++] = 0x82; }
                } else if (data[1] == 0xB0 && dataLen >= 5) {
                    // READ BINARY: offset p1p2, length Le
                    uint16_t off = ((uint16_t)data[2] << 8) | data[3];
                    uint8_t le = data[4];
                    if (le == 0) le = 16;
                    const uint8_t* src = nullptr;
                    uint16_t srcLen = 0;
                    if (t4tFile == 1) { src = t4tCc; srcLen = sizeof(t4tCc); }
                    else if (t4tFile == 2) {
                        // NDEF file = 2-byte length prefix + message
                        uint8_t hdr[2] = {(uint8_t)(ndefLen_ >> 8), (uint8_t)(ndefLen_ & 0xFF)};
                        // emulate a virtual file of hdr+message
                        for (uint8_t i = 0; i < le && rl + 2 < sizeof(resp); i++) {
                            uint16_t fp = off + i;
                            uint8_t b = fp < 2 ? hdr[fp] : (fp - 2 < ndefLen_ ? ndefMsg[fp - 2] : 0);
                            resp[rl++] = b;
                        }
                        resp[rl++] = 0x90; resp[rl++] = 0x00;
                        Nfc::emulateRespond(resp, (uint8_t)rl);
                        Features::requestRedraw();
                        return;
                    } else { resp[rl++] = 0x6A; resp[rl++] = 0x82; }
                    if (handled && t4tFile == 1) {
                        for (uint8_t i = 0; i < le && off + i < srcLen && rl + 2 < sizeof(resp); i++)
                            resp[rl++] = src[off + i];
                        resp[rl++] = 0x90; resp[rl++] = 0x00;
                    }
                } else handled = false;
                if (handled && rl) Nfc::emulateRespond(resp, (uint8_t)rl);
            }
            Features::requestRedraw();
        } else {
            pollFailures++;
            if (pollFailures > 20) {
                // ридер ушёл / чип заглох — перезапускаем эмуляцию
                Nfc::emulateStop();
                Nfc::emulateStart(emuUid, emuUidLen, emuSak);
                pollFailures = 0;
            }
        }
    }

    void onExit() {
        if (state == Emulating) Nfc::emulateStop();
        state = Select;
    }
}

REGISTER_FEATURE("Emulate", "NFC", MwIcon::Lock, 40,
                 nullptr, input, tick, draw, onExit);
