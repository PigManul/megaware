// =====================================================================
//  NFC/iButton: DS1990 emulator — NO 125 kHz hardware needed.
//  An iButton key is just a 1-Wire chip: the EMULATION is pure GPIO
//  timing. Wire a probe contact to GPIO 17 (and GND); the reader panel
//  pulls the line up (use its own pull-up, or 2-4.7 kOhm to 3.3 V).
//  Source: /ibtn/*.ibtn (Flipper format: "Rom Data: FF FF FF FF FF FF FF FF").
//  Emulated: DS1990 READ ROM (0x33): RESET -> presence -> 64-bit ROM.
//  Rotation: pick the file, click: emulate, Quit: stop.
// =====================================================================
#include "../ui/features.h"
#include "../storage/files.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { Select, Emulating };
    State state = Select;
    int selIndex = 0;
    char statusMsg[48] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    uint8_t rom[8] = {0};
    char romStr[24] = "";
    bool crcOk = false;
    uint32_t reads = 0;

    constexpr int IB_PIN = 17;

    fs::FS& ibFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    uint8_t dallasCrc8(const uint8_t* d, int n) {
        uint8_t crc = 0;
        while (n--) {
            uint8_t b = *d++;
            for (int i = 0; i < 8; i++) {
                uint8_t mix = (crc ^ b) & 0x01;
                crc >>= 1;
                if (mix) crc ^= 0x8C;
                b >>= 1;
            }
        }
        return crc;
    }

    bool loadIbtn(const char* path) {
        File f = ibFs().open(path, FILE_READ);
        if (!f) return false;
        bool ok = false;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.startsWith("Rom Data:")) {
                String hex = line.substring(9);
                hex.trim();
                for (int i = 0; i < 8; i++) {
                    if (hex.length() < (size_t)(i * 2 + 2)) break;
                    char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                    rom[i] = (uint8_t)strtol(b, nullptr, 16);
                }
                crcOk = rom[7] == dallasCrc8(rom, 7);
                ok = true;
                break;
            }
        }
        f.close();
        if (ok) {
            char* p = romStr;
            for (int i = 0; i < 8 && p - romStr + 3 < (int)sizeof(romStr); i++)
                p += snprintf(p, 3, "%02X", rom[i]);
            *p = 0;
        }
        return ok;
    }

    // ---- 1-Wire device emulation (DS1990 READ ROM) ----
    // All times in us, per the Maxim 1-Wire spec.
    bool owIdleHigh() { return digitalRead(IB_PIN) == HIGH; }

    void owWaitReset() {
        uint32_t lowStart = 0;
        while (true) {
            if (!owIdleHigh()) {
                if (!lowStart) lowStart = micros();
                else if (micros() - lowStart > 480) { // RESET
                    // wait for release
                    while (!owIdleHigh()) delayMicroseconds(10);
                    delayMicroseconds(40);
                    // presence pulse
                    pinMode(IB_PIN, OUTPUT);
                    digitalWrite(IB_PIN, LOW);
                    delayMicroseconds(210);
                    pinMode(IB_PIN, INPUT_PULLUP);
                    return;
                }
            } else lowStart = 0;
            delayMicroseconds(20);
        }
    }

    uint8_t owReadCommand() {
        uint8_t cmd = 0;
        for (int bit = 0; bit < 8; bit++) {
            // wait for the master to start a slot (falling edge)
            uint32_t t0 = micros();
            while (owIdleHigh()) {
                if (micros() - t0 > 20000) return 0xFF; // master stopped talking
            }
            delayMicroseconds(30); // sample window
            if (owIdleHigh()) cmd |= (1 << bit); // released = 1
            // wait for slot end
            uint32_t t1 = micros();
            while (!owIdleHigh() && micros() - t1 < 200) {}
            delayMicroseconds(10);
        }
        return cmd;
    }

    void owWriteBit0() {
        pinMode(IB_PIN, OUTPUT);
        digitalWrite(IB_PIN, LOW);
        delayMicroseconds(60);
        pinMode(IB_PIN, INPUT_PULLUP);
    }

    void owSendRom() {
        for (int bit = 0; bit < 64; bit++) {
            // wait for the read slot start
            uint32_t t0 = micros();
            while (owIdleHigh()) {
                if (micros() - t0 > 20000) return; // master gone
            }
            uint8_t b = (rom[bit / 8] >> (bit % 8)) & 1;
            if (!b) {
                delayMicroseconds(25);
                owWriteBit0();
                delayMicroseconds(40);
            } else {
                delayMicroseconds(65); // do nothing: line stays high = 1
            }
        }
    }

    void emulateLoop() {
        pinMode(IB_PIN, INPUT_PULLUP);
        while (true) {
            owWaitReset();
            reads++;
            uint8_t cmd = owReadCommand();
            if (cmd == 0x33 || cmd == 0x0F) { // READ ROM / SKIP ROM variant
                owSendRom();
            } else if (cmd == 0xFF) {
                return; // master went away
            }
            Features::requestRedraw();
            if (reads % 8 == 0) return; // re-enter from tick: keep the UI alive
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, state == Select ? "IBUTTON" : "IBUTTON EMULATING");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Select) {
            FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
            int n = Files::listCount(root, "/ibtn");
            int visible = (h - 46) / Theme::ROW_LIST;
            int selC = n ? (selIndex % n + n) % n : 0;
            int top = selC - visible + 1;
            if (top < 0) top = 0;
            c.setTextDatum(textdatum_t::middle_left);
            for (int i = 0; i < visible && top + i < n; i++) {
                FsEntry e = Files::listEntry(root, "/ibtn", top + i);
                bool s = (top + i == selC);
                int yy = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
                if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, yy); }
                c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
                c.drawString(e.name, x0 + 12, yy);
            }
            if (n == 0) {
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString("no keys", x0 + 12, y0 + 20);
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
                c.drawString("upload /ibtn/*.ibtn (Flipper)", x0 + 12, y0 + 38);
            } else {
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
                c.drawString("click: emulate on GPIO17", x0 + 12, y0 + h - 22);
            }
        } else {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("DS1990 EMULATING", x0 + 10, y0 + 22);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(romStr, x0 + 10, y0 + 40);
            c.setTextColor(crcOk ? Theme::OK : Theme::WARN, Theme::BG);
            c.drawString(crcOk ? "CRC OK" : "CRC BAD", x0 + 10, y0 + 54);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            char line[32];
            snprintf(line, sizeof(line), "reader reads: %lu", (unsigned long)reads);
            c.drawString(line, x0 + 10, y0 + 70);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("wire: GPIO17 + GND to the reader", x0 + 10, y0 + h - 40);
            c.drawString("pad; external 2-4.7k pull-up", x0 + 10, y0 + h - 30);
            c.drawString("quit: stop", x0 + 10, y0 + h - 14);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 18);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/ibtn");
        if (state == Select) {
            if (in.rotationSteps != 0 && n > 0) {
                selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, "/ibtn", selC);
                String path = String("/ibtn/") + e.name;
                if (!loadIbtn(path.c_str())) {
                    setStatus("parse failed (no Rom Data)", false);
                    Features::requestRedraw();
                    return true;
                }
                reads = 0;
                state = Emulating;
                Features::requestRedraw();
                return true;
            }
            return false;
        }
        // Emulating: click stops
        if (in.event == InputEvent::Click) {
            state = Select;
            setStatus("stopped", true);
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Emulating) return;
        emulateLoop(); // returns periodically to keep the UI alive
    }

    void onExit() { if (state == Emulating) state = Select; }
}

REGISTER_FEATURE("iButton", "NFC", MwIcon::Lock, 60,
                 nullptr, input, tick, draw, onExit);
