// =====================================================================
//  NFC: Emulate — устройство притворяется картой с UID из дампа.
//  Выбор дампа из Saved → PN532 входит в target-режим → чужой ридер
//  видит твой UID. Экран: статус эмуляции, счётчик обращений ридера.
//
//  Работает ТОЛЬКО на ридерах, проверяющих UID без CRYPTO1-auth
//  (дешёвые домофоны, турникеты). MIFARE data-emulation невозможна:
//  ISO14443-3A FDT ~86 мкс, а шина I2C + ESP32 медленнее.
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
        snprintf(path, sizeof(path), "/rfid/%s.bin", base);

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
        emuUidLen = emuUidLen; // уже установлен
        emuHits = 0;
        pollFailures = 0;
        state = Emulating;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "EMULATE");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Select) {
            FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
            int n = Files::listCount(root, "/rfid");
            int visible = (h - 40) / Theme::ROW_LIST;
            int selC = n ? (selIndex % n + n) % n : 0;
            int top = selC - visible + 1;
            if (top < 0) top = 0;
            c.setTextDatum(textdatum_t::middle_left);
            for (int i = 0; i < visible && top + i < n; i++) {
                int idx = top + i;
                FsEntry e = Files::listEntry(root, "/rfid", idx);
                bool s = (idx == selC);
                int yy = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
                if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, yy); }
                c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
                c.drawString(e.name, x0 + 12, yy);
            }
            if (n == 0) {
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString("no dumps — use Dump first", x0 + 12, y0 + 20);
            }
            c.setTextDatum(textdatum_t::top_left);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: emulate selected UID", x0 + 10, y0 + h - 14);
            return;
        }

        // Emulating state
        if (emuHits > 0) {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString("READER DETECTED", x0 + 10, y0 + 26);
        } else {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("EMULATING...", x0 + 10, y0 + 26);
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
            int n = Files::listCount(root, "/rfid");
            if (in.rotationSteps != 0 && n > 0) {
                selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, "/rfid", selC);
                snprintf(openName, sizeof(openName), "%s", e.name);
                startEmulate();
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
