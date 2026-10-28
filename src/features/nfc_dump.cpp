// =====================================================================
//  NFC: Dump — чтение карты дефолтными ключами (MIFARE Classic 1K/4K:
//  перебор ключей по секторам; NTAG21x/Ultralight: постранично, без
//  ключей), сохранение дампа в /rfid/<uid>.bin (SD, иначе Flash) и
//  hex-вьюер прямо в фиче. Дамп инкрементальный (по куску за тик) —
//  GUI остаётся отзывчивым, прогресс рисуется живьём.
//
//  Управление: клик — начать; вращение — листать дамп; клик в вьюере —
//  заново обнаружить карту.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { Idle, Dumping, Viewer };
    State state = Idle;

    // Буфер дампа — в PSRAM (4K-карта = 4 КБ, внутренний heap дороже)
    static EXT_RAM_ATTR uint8_t dumpBuf[4096];
    static bool blockOk[256];          // авторизация/чтение прошли (по блоку/странице)
    Nfc::Tag tag;
    bool isClassic = false;
    uint16_t totalUnits = 0;           // Classic: блоки; NTAG: страницы
    uint16_t doneUnits = 0;
    uint16_t viewOffset = 0;           // первый блок/страница вверху вьюера
    char uidText[40] = "";
    char savePath[48] = "";
    bool savedOk = false;

    void uidToText() {
        uidText[0] = 0;
        size_t pos = 0;
        for (uint8_t i = 0; i < tag.uidLen && pos + 3 < sizeof(uidText); i++) {
            if (i && i % 2 == 0 && pos + 1 < sizeof(uidText)) uidText[pos++] = ' ';
            pos += snprintf(uidText + pos, sizeof(uidText) - pos, "%02X", tag.uid[i]);
        }
    }

    void prepare(const Nfc::Tag& t) {
        tag = t;
        isClassic = t.isClassic;
        totalUnits = t.isClassic ? t.blocks : t.pages;
        doneUnits = 0;
        viewOffset = 0;
        memset(blockOk, 0, sizeof(blockOk));
        uidToText();
        Nfc::loadKeyDictionary(); // /rfid/keys.txt — пользовательские ключи
        state = Dumping;
    }

    void saveDump() {
        if (Files::sdPresent()) SD.mkdir("/rfid");
        else LittleFS.mkdir("/rfid");
        snprintf(savePath, sizeof(savePath), "/rfid/%s.bin", uidText);
        for (char* p = savePath; *p; p++) if (*p == ' ') *p = '-'; // пробелы в UID-тексте
        fs::FS& fs = Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS;
        File f = fs.open(savePath, FILE_WRITE);
        uint32_t bytes = (uint32_t)totalUnits * 4; // страница = 4Б; блок Classic = 4 страницы
        if (isClassic) bytes = (uint32_t)totalUnits * 16;
        savedOk = f && f.write(dumpBuf, bytes) == bytes;
        if (f) f.close();

        // sidecar-карта ключей для Classic (6 байт на сектор; 0xFF*6 = fail)
        if (isClassic && savedOk) {
            char kpath[64];
            snprintf(kpath, sizeof(kpath), "/rfid/%s.kmap", uidText);
            for (char* p = kpath; *p; p++) if (*p == ' ') *p = '-';
            File kf = fs.open(kpath, FILE_WRITE);
            if (kf) {
                uint8_t sectorKey[40][6];
                memset(sectorKey, 0xFF, sizeof(sectorKey));
                for (int u = 0; u < (int)totalUnits && u < 256; u++) {
                    uint8_t sector = (u < 128) ? u / 4 : 32 + (u - 128) / 16;
                    if (blockOk[u]) Nfc::lastAuthKey(sectorKey[sector]);
                }
                int sectors = totalUnits / 4;
                kf.write((uint8_t*)sectorKey, sectors * 6);
                kf.close();
            }
        }
    }

    void drawProgress(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        char line[48];
        snprintf(line, sizeof(line), "Reading %d / %d", doneUnits, totalUnits);
        c.setTextColor(Theme::ACCENT, Theme::BG_PANEL);
        c.drawString(line, x0 + 10, y0 + 26);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
        c.drawString(uidText, x0 + 10, y0 + 44);
        float pct = totalUnits ? (float)doneUnits / totalUnits : 0.0f;
        c.fillRect(x0 + 10, y0 + h - 14, (int)((w - 20) * pct), 4, Theme::ACCENT);
    }

    void drawHexRow(LGFX_Sprite& c, int x, int y, uint16_t unit, uint8_t half) {
        const uint8_t* src;
        uint8_t count;
        if (isClassic) src = dumpBuf + (uint32_t)unit * 16 + half * 8, count = 8;
        else src = dumpBuf + (uint32_t)unit * 4, count = 4;
        bool ok = isClassic ? blockOk[unit] : blockOk[unit];
        char line[40];
        int pos = snprintf(line, sizeof(line), "%03X.%c", unit, half ? '1' : '0');
        for (uint8_t i = 0; i < count; i++) pos += snprintf(line + pos, sizeof(line) - pos, " %02X", src[i]);
        c.setTextColor(ok ? Theme::TEXT_DIM : Theme::ERR, Theme::BG_PANEL);
        c.drawString(line, x, y);
    }

    void drawViewer(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
        c.drawString(tag.type, x0 + 10, y0 + 22);
        char line[64];
        snprintf(line, sizeof(line), "UID %s", uidText);
        c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
        c.drawString(line, x0 + 10, y0 + 36);

        int rows = (h - 66) / 13; // строк hex-дампа на экране
        if (isClassic) rows /= 2; // блок 16Б = 2 строки по 8Б
        for (int i = 0; i < rows; i++) {
            uint16_t unit = viewOffset + (isClassic ? i / 2 : i);
            if (unit >= totalUnits) break;
            int y = y0 + 52 + i * 13;
            if (isClassic) drawHexRow(c, x0 + 10, y, unit, i % 2);
            else drawHexRow(c, x0 + 10, y, unit, 0);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
        snprintf(line, sizeof(line), "%d/%d  rotate: scroll  click: rescan%s",
                 viewOffset, totalUnits - 1, savedOk ? "" : "  (save FAILED)");
        c.drawString(line, x0 + 10, y0 + h - 14);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NFC DUMP");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Idle) {
            if (!Nfc::busPresent()) {
                c.setTextColor(Theme::ERR, Theme::BG_PANEL);
                c.drawString("PN532 not on bus (I2C 0x24)", x0 + 10, y0 + 26);
                return;
            }
            if (!Nfc::ensure()) {
                c.setTextColor(Theme::ERR, Theme::BG_PANEL);
                c.drawString("PN532 found, no response", x0 + 10, y0 + 26);
                return;
            }
            c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
            c.drawString("Default-key dump, then /rfid/*.bin", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("hold card to the back, click: start", x0 + 10, y0 + 44);
        } else if (state == Dumping) {
            drawProgress(c, x0, y0, w, h);
        } else {
            drawViewer(c, x0, y0, w, h);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (state == Viewer) {
                state = Idle; // заново искать карту
            } else if (state == Idle) {
                Nfc::Tag t;
                if (Nfc::readTag(t)) {
                    prepare(t);
                    Features::requestRedraw();
                    return true;
                }
            }
            Features::requestRedraw();
            return true;
        }
        if (in.rotationSteps != 0 && state == Viewer) {
            int v = viewOffset + in.rotationSteps;
            if (v < 0) v = 0;
            if (v >= totalUnits) v = totalUnits - 1;
            viewOffset = (uint16_t)v;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Dumping) return;

        // Один блок/страницу за тик: auth+read Classic ~10-30 мс, GUI
        // между тиками остаётся отзывчивым, прогресс виден живьём.
        uint16_t u = doneUnits;
        if (isClassic) {
            blockOk[u] = Nfc::classicReadBlock((uint8_t)u, dumpBuf + (uint32_t)u * 16);
            if (!blockOk[u]) memset(dumpBuf + (uint32_t)u * 16, 0, 16);
        } else {
            blockOk[u] = Nfc::ntagReadPage((uint8_t)u, dumpBuf + (uint32_t)u * 4);
            if (!blockOk[u]) memset(dumpBuf + (uint32_t)u * 4, 0, 4);
        }
        doneUnits++;
        if (doneUnits % 4 == 0 || doneUnits >= totalUnits) Features::requestRedraw();

        if (doneUnits >= totalUnits) {
            saveDump();
            state = Viewer;
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Dump", "NFC", MwIcon::Chip, 20,
                 nullptr, input, tick, draw, nullptr);
