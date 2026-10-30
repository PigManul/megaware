// =====================================================================
//  NFC: Key Hunter — MIFARE Classic dictionary attack (authorized
//  cards only). Walks every sector x Key A/Key B against the key
//  dictionary: built-in defaults + /rfid/keys.txt + /rfid/dict*.txt
//  (user drops big dictionaries on the card via WebUI, one 12-hex-char
//  key per line). Found keys are appended to /rfid/keys.txt, so Dump
//  immediately reads deeper. Progress + live results on screen.
//  Click: start / pause. Long press: reset results.
//  Honest scope: dictionary and default-key recovery only. Crypto1
//  math attacks (darkside/nested) are NOT in this firmware.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State { Idle, Working, Done, Paused };
    State state = Idle;

    constexpr int MAX_KEYS = 600;
    uint8_t keys[MAX_KEYS][6];
    int keyCount = 0;
    char statusMsg[56] = "";

    int totalSectors = 16;
    int curSector = 0;
    int curKey = 0;
    int curKeyB = false; // 0 = key A pass, 1 = key B pass
    uint32_t triedCount = 0;

    struct Found { uint8_t sector; bool keyB; uint8_t key[6]; };
    constexpr int MAX_FOUND = 40;
    Found foundList[MAX_FOUND];
    int foundCount = 0;

    void saveFound() {
        File f = SD.exists("/rfid/keys.txt") ? SD.open("/rfid/keys.txt", FILE_APPEND)
                                             : LittleFS.open("/rfid/keys.txt", FILE_APPEND);
        if (!f) {
            if (!SD.exists("/rfid")) SD.mkdir("/rfid");
            f = SD.open("/rfid/keys.txt", FILE_APPEND);
        }
        if (!f) return;
        for (int i = 0; i < foundCount; i++) {
            char hex[13];
            for (int j = 0; j < 6; j++) snprintf(hex + j * 2, 3, "%02X", foundList[i].key[j]);
            f.printf("%s %s %d\n", hex, foundList[i].keyB ? "B" : "A", foundList[i].sector);
        }
        f.close();
    }

    bool alreadyFound(uint8_t sector, bool keyB) {
        for (int i = 0; i < foundCount; i++)
            if (foundList[i].sector == sector && foundList[i].keyB == keyB) return true;
        return false;
    }

    void loadDicts() {
        keyCount = 0;
        Nfc::loadKeyDictionary(); // warms the HAL's own list (defaults + keys.txt)
        const char* files[4] = {"/rfid/keys.txt", "/rfid/dict1.txt", "/rfid/dict2.txt", "/rfid/dict3.txt"};
        fs::FS* fss[2] = {&SD, &LittleFS};
        for (int fi = 0; fi < 4 && keyCount < MAX_KEYS; fi++) {
            for (int fs = 0; fs < 2 && keyCount < MAX_KEYS; fs++) {
                if (!fss[fs]->exists(files[fi])) continue;
                File f = fss[fs]->open(files[fi]);
                if (!f) continue;
                while (f.available() && keyCount < MAX_KEYS) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    int sp = line.indexOf(' ');
                    if (sp > 0) line = line.substring(0, sp); // "AABBCCDDEEFF B 5" -> key only
                    if (line.length() != 12) continue;
                    uint8_t key[6];
                    bool ok = true;
                    for (int i = 0; i < 6; i++) {
                        char b[3] = {line[i * 2], line[i * 2 + 1], 0};
                        key[i] = (uint8_t)strtol(b, nullptr, 16);
                    }
                    (void)ok;
                    bool dup = false;
                    for (int k = 0; k < keyCount; k++)
                        if (memcmp(keys[k], key, 6) == 0) { dup = true; break; }
                    if (dup) continue;
                    memcpy(keys[keyCount++], key, 6);
                }
                f.close();
            }
        }
    }

    // one auth attempt per tick, non-blocking-ish UI
    void step() {
        if (curSector >= totalSectors) {
            state = Done;
            saveFound();
            snprintf(statusMsg, sizeof(statusMsg), "done: %d keys found, saved", foundCount);
            return;
        }
        uint8_t trailerBlock = (curSector < 32) ? curSector * 4 + 3 : 128 + (curSector - 32) * 16 + 15;
        triedCount++;
        if (!alreadyFound(curSector, curKeyB)) {
            if (Nfc::classicTryKey(trailerBlock, keys[curKey], !curKeyB)) {
                if (foundCount < MAX_FOUND) {
                    foundList[foundCount].sector = curSector;
                    foundList[foundCount].keyB = curKeyB;
                    memcpy(foundList[foundCount].key, keys[curKey], 6);
                    foundCount++;
                }
                Features::requestRedraw();
            }
        }
        // advance
        if (++curKey >= keyCount) {
            curKey = 0;
            if (curKeyB) {
                curKeyB = 0;
                curSector++;
                if (curSector == 16 && totalSectors > 16) curSector = 16; // 1K/4K boundary
            } else {
                curKeyB = 1;
            }
        }
        static uint32_t lastDraw = 0;
        if (millis() - lastDraw > 400) {
            lastDraw = millis();
            Features::requestRedraw();
        }
    }

    void start() {
        Nfc::Tag tag;
        if (!Nfc::ensure() || !Nfc::readTag(tag)) {
            snprintf(statusMsg, sizeof(statusMsg), "no card in the field");
            return;
        }
        if (!tag.isClassic) {
            snprintf(statusMsg, sizeof(statusMsg), "not a MIFARE Classic (%s)", tag.type);
            return;
        }
        totalSectors = tag.blocks > 64 ? 40 : 16;
        loadDicts();
        if (keyCount == 0) {
            snprintf(statusMsg, sizeof(statusMsg), "no keys in dictionaries");
            return;
        }
        curSector = 0;
        curKey = 0;
        curKeyB = 0;
        triedCount = 0;
        state = Working;
        snprintf(statusMsg, sizeof(statusMsg), "%d keys x %d sectors x A/B", keyCount, totalSectors);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "KEY HUNTER (MFC)");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[56];

        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("authorized cards only - dictionary attack", x0 + 10, y0 + 22);

        if (state == Idle) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: place a card and start", x0 + 10, y0 + 44);
        } else {
            c.setTextColor(state == Done ? Theme::OK : (state == Paused ? Theme::WARN : Theme::ACCENT), Theme::BG);
            c.drawString(state == Done ? "DONE" : (state == Paused ? "PAUSED" : "RUNNING"), x0 + 10, y0 + 40);
            int total = totalSectors * 2 * keyCount;
            int done = triedCount;
            int barW = (w - 20) * (total ? (done * 100 / total) : 0) / 100;
            c.drawRect(x0 + 10, y0 + 54, w - 20, 8, Theme::TEXT_DIM);
            c.fillRect(x0 + 10, y0 + 54, barW, 8, Theme::ACCENT);
            c.setTextColor(Theme::TEXT, Theme::BG);
            snprintf(line, sizeof(line), "sector %d/%d  key %d/%d  %s-pass",
                     curSector + 1, totalSectors, curKey + 1, keyCount, curKeyB ? "B" : "A");
            c.drawString(line, x0 + 10, y0 + 70);
            c.setTextColor(Theme::OK, Theme::BG);
            snprintf(line, sizeof(line), "found: %d", foundCount);
            c.drawString(line, x0 + 10, y0 + 86);
            // last found keys
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            int show = foundCount < 4 ? foundCount : 4;
            for (int i = 0; i < show; i++) {
                const Found& f = foundList[foundCount - show + i];
                char hex[13];
                for (int j = 0; j < 6; j++) snprintf(hex + j * 2, 3, "%02X", f.key[j]);
                snprintf(line, sizeof(line), "s%02d %s %s", f.sector, hex, f.keyB ? "B" : "A");
                c.drawString(line, x0 + 20, y0 + 102 + i * Theme::ROW_LIST);
            }
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 30);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: start/pause   hold: clear", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (state == Idle || state == Done) {
                foundCount = 0;
                start();
            } else if (state == Working) {
                state = Paused;
            } else if (state == Paused) {
                state = Working;
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            foundCount = 0;
            state = Idle;
            statusMsg[0] = 0;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state == Working) step();
    }
}

REGISTER_FEATURE("Key Hunter", "NFC", MwIcon::Lock, 30,
                 nullptr, input, tick, draw, nullptr);
