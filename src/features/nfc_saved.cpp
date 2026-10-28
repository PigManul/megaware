// =====================================================================
//  NFC: Saved — библиотека дампов (/rfid/*.bin, SD иначе Flash).
//  Viewer: вращение — скролл, клик — вход в режим правки байта,
//  hold — меню [Write to card / Export .nfc / Export .mfd / Key map].
//  Edit: вращение — ±1 к байту, клик — следующий байт, hold — выход.
//  Write to card: изменённые блоки пишутся в ПРИСУТСТВУЮЩУЮ метку
//  (ключ — из словаря/дефолтных, как при дампе).
//  Exports: .nfc (Flipper) и .mfd (raw) пишутся в /rfid/.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { List, View, Edit, Menu, KeyMap };
    State state = List;

    static EXT_RAM_ATTR uint8_t fileBuf[4096];
    static bool blockDirty[256];        // блок/страница изменён в редакторе
    uint32_t fileBytes = 0;
    bool classicDump = false;           // .bin от MIFARE Classic (1K/4K)
    uint16_t blocksTotal = 0;           // classic: блоки (64/256); ntag: страницы
    uint16_t viewOffset = 0;            // первый байт вверху вьюера
    int editCursor = 0;                 // байт под курсором в Edit
    char openName[64] = "";
    char statusMsg[48] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    int menuSel = 0;
    const char* const MENU[4] = {"Write to card", "Export .nfc", "Export .mfd", "Key map"};

    int listSel = 0;

    fs::FS& dumpFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    void baseName(char* out, size_t n) {
        snprintf(out, n, "%s", openName);
        char* dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }

    bool loadDump(const char* name) {
        String path = String("/rfid/") + name;
        File f = dumpFs().open(path, FILE_READ);
        if (!f) { setStatus("open failed", false); return false; }
        fileBytes = f.size();
        if (fileBytes > sizeof(fileBuf)) fileBytes = sizeof(fileBuf);
        fileBytes = f.read(fileBuf, fileBytes);
        f.close();
        classicDump = (fileBytes == 1024 || fileBytes == 4096);
        blocksTotal = classicDump ? (uint16_t)(fileBytes / 16) : (uint16_t)(fileBytes / 4);
        memset(blockDirty, 0, sizeof(blockDirty));
        snprintf(openName, sizeof(openName), "%s", name);
        viewOffset = 0;
        editCursor = 0;
        return fileBytes > 0;
    }

    // ---------- экспорт ----------
    void exportNfc() {
        char base[48]; baseName(base, sizeof(base));
        String path = String("/rfid/") + base + ".nfc";
        File f = dumpFs().open(path, FILE_WRITE);
        if (!f) { setStatus("export failed", false); return; }
        f.println("Filetype: Flipper NFC device");
        f.println("Version: 4");
        f.println("# Device type can be ISO14443-3A, ISO14443-4A, ISO14443-3B, ISO14443-4B");
        f.println("Device type: ISO14443-3A");
        f.println("# UID is common for all formats");
        f.print("UID:");
        for (int i = 0; i < 4 && i < 16; i++) f.printf(" %02X", fileBuf[i]);
        f.println();
        f.println("# Mifare Classic specific data");
        f.println("Mifare version: 1.0");
        f.printf("Blocks total: %u\n", blocksTotal);
        f.printf("Blocks read: %u\n", blocksTotal);
        f.printf("Pages read: %u\n", blocksTotal);
        f.println("Failed authentication attempts: 0");
        for (uint16_t b = 0; b < blocksTotal; b++) {
            f.printf("Block %u:", b);
            for (int i = 0; i < 16; i++) f.printf(" %02X", fileBuf[b * 16 + i]);
            f.println();
        }
        f.close();
        Files::invalidateCache();
        setStatus("exported .nfc", true);
    }

    void exportMfd() {
        char base[48]; baseName(base, sizeof(base));
        String path = String("/rfid/") + base + ".mfd";
        File f = dumpFs().open(path, FILE_WRITE);
        if (!f) { setStatus("export failed", false); return; }
        f.write(fileBuf, fileBytes);
        f.close();
        Files::invalidateCache();
        setStatus("exported .mfd", true);
    }

    // ---------- запись изменённых блоков в метку ----------
    void writeBackToCard() {
        Nfc::Tag tag;
        if (!Nfc::readTag(tag)) { setStatus("no card in field", false); return; }
        if (classicDump && !tag.isClassic) { setStatus("not a Classic card", false); return; }
        if (!classicDump && !tag.isNtag) { setStatus("not an NTAG/UL", false); return; }

        int okN = 0, failN = 0;
        for (int u = 0; u < (int)blocksTotal; u++) {
            if (!blockDirty[u]) continue;
            bool ok;
            if (classicDump) ok = Nfc::classicWriteBlock((uint8_t)u, fileBuf + (uint32_t)u * 16);
            else ok = Nfc::ntagWritePage((uint8_t)u, fileBuf + (uint32_t)u * 4);
            if (ok) okN++;
            else {
                failN++;
                if (failN > 8) break; // карта ушла/заглохла — не спамим
            }
        }
        char m[48];
        snprintf(m, sizeof(m), "card: %d ok, %d fail", okN, failN);
        setStatus(m, failN == 0);
    }

    // ---------- отрисовки ----------
    void drawList(LGFX_Sprite& c, int x0, int y0, int w, int h, int sel) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/rfid");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        int visible = (h - Theme::ROW_LIST) / Theme::ROW_LIST;
        int selC = n ? (sel % n + n) % n : 0;
        int top = selC;
        if (top > n - visible) top = n - visible;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            int idx = top + i;
            FsEntry e = Files::listEntry(root, "/rfid", idx);
            bool s = (idx == selC);
            int y = y0 + Theme::ROW_LIST + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no dumps yet", x0 + 12, y0 + Theme::ROW_LIST + 6);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Dump writes to /rfid/<uid>.bin", x0 + 12, y0 + Theme::ROW_LIST + 22);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 4);
        }
    }

    void drawHexRows(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        int rows = (h - 30) / Theme::ROW_LIST;
        for (int i = 0; i < rows; i++) {
            uint32_t off = viewOffset + (uint32_t)i * 8;
            if (off >= fileBytes) break;
            char line[48];
            int pos = snprintf(line, sizeof(line), "%04X", (unsigned)off);
            for (int b = 0; b < 8 && off + b < fileBytes; b++)
                pos += snprintf(line + pos, sizeof(line) - pos, " %02X", fileBuf[off + b]);
            bool inEdit = (state == Edit && editCursor >= (int)off && editCursor < (int)off + 8);
            c.setTextColor(inEdit ? Theme::WARN : Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 12, y0 + 30 + i * Theme::ROW_LIST);
        }
    }

    void drawViewer(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(openName, x0 + 12, y0 + 8);
        int edits = 0;
        for (int i = 0; i < (int)blocksTotal; i++) if (blockDirty[i]) edits++;
        char info[48];
        snprintf(info, sizeof(info), "%lu B  %s%s", (unsigned long)fileBytes,
                 classicDump ? "classic" : "ntag", edits ? "  EDITED" : "");
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(info, x0 + w - 4, y0 + 8);
        drawHexRows(c, x0, y0, w, h);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(state == Edit ? "rot: +-1  click: next  hold: done"
                                   : "click: edit  hold: menu",
                     x0 + 12, y0 + h - 4);
    }

    void drawMenu(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        for (int i = 0; i < 4; i++) {
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            bool s = (i == menuSel);
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(MENU[i], x0 + 12, y);
        }
    }

    void drawKeyMap(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString("KEY MAP (sectors)", x0 + 12, y0 + 22);

        char path[96];
        char base[48]; baseName(base, sizeof(base));
        snprintf(path, sizeof(path), "/rfid/%s.kmap", base);
        File f = dumpFs().open(path, FILE_READ);
        if (!f) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no key map (dump again)", x0 + 12, y0 + 42);
            return;
        }
        uint16_t sectors = blocksTotal / 4;
        int cell = 14, cols = 8;
        for (uint16_t s = 0; s < sectors && s < 40; s++) {
            uint8_t key[6];
            f.seek((uint32_t)s * 6);
            if (f.read(key, 6) != 6) break;
            bool isDef = memcmp(key, "\xFF\xFF\xFF\xFF\xFF\xFF", 6) == 0;
            bool none = true;
            for (int i = 0; i < 6; i++) if (key[i] != 0xFF) none = false;
            uint16_t col = none ? Theme::ERR : (isDef ? Theme::OK : Theme::WARN);
            int cx = x0 + 12 + (s % cols) * cell;
            int cy = y0 + 44 + (s / cols) * cell;
            c.fillRect(cx, cy, cell - 2, cell - 2, col);
        }
        f.close();
        int ly = y0 + 44 + ((sectors + 7) / 8) * cell + 8;
        c.setTextDatum(textdatum_t::middle_left);
        c.setTextColor(Theme::OK, Theme::BG);
        c.drawString("default", x0 + 12, ly);
        c.setTextColor(Theme::WARN, Theme::BG);
        c.drawString("dict", x0 + 54, ly);
        c.setTextColor(Theme::ERR, Theme::BG);
        c.drawString("failed", x0 + 94, ly);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: back", x0 + 12, y0 + h - 4);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, state == List ? "SAVED DUMPS" :
                     state == Menu ? "ACTIONS" : state == KeyMap ? "KEY MAP" : "SAVED DUMPS");
        switch (state) {
            case List: drawList(c, x0, y0, w, h, listSel); break;
            case View: drawViewer(c, x0, y0, w, h); break;
            case Edit: drawViewer(c, x0, y0, w, h); break;
            case Menu: drawMenu(c, x0, y0, w, h); break;
            case KeyMap: drawKeyMap(c, x0, y0, w, h); break;
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;

        if (state == List) {
            int n = Files::listCount(root, "/rfid");
            if (in.rotationSteps != 0 && n > 0) {
                listSel = ((listSel + in.rotationSteps) % n + n) % n;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (listSel % n + n) % n;
                FsEntry e = Files::listEntry(root, "/rfid", selC);
                if (loadDump(e.name)) state = View;
                Features::requestRedraw();
                return true;
            }
            return false;
        }

        if (state == View) {
            if (in.rotationSteps != 0) {
                int v = viewOffset + in.rotationSteps * 8;
                if (v < 0) v = 0;
                if ((uint32_t)v > fileBytes) v = fileBytes;
                viewOffset = (uint16_t)v;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click) { state = Edit; Features::requestRedraw(); return true; }
            if (in.event == InputEvent::LongPress) { state = Menu; menuSel = 0; Features::requestRedraw(); return true; }
            return false;
        }

        if (state == Edit) {
            if (in.rotationSteps != 0) {
                int v = fileBuf[editCursor] + in.rotationSteps;
                fileBuf[editCursor] = (uint8_t)constrain(v, 0, 255);
                if (classicDump) blockDirty[editCursor / 16] = true;
                else blockDirty[editCursor / 4] = true;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click) {
                editCursor = (editCursor + 1) % (int)fileBytes;
                if ((uint32_t)editCursor < viewOffset) viewOffset = (uint16_t)editCursor;
                if ((uint32_t)editCursor >= viewOffset + 40) viewOffset = (uint16_t)(editCursor - 32);
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress) { state = View; Features::requestRedraw(); return true; }
            return false;
        }

        if (state == Menu) {
            if (in.rotationSteps != 0) {
                menuSel = ((menuSel + in.rotationSteps) % 4 + 4) % 4;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click) {
                if (menuSel == 0) writeBackToCard();
                else if (menuSel == 1) exportNfc();
                else if (menuSel == 2) exportMfd();
                else state = KeyMap;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress || in.event == InputEvent::UserKey) {
                state = View;
                Features::requestRedraw();
                return true;
            }
            return false;
        }

        if (state == KeyMap) {
            if (in.event == InputEvent::Click || in.event == InputEvent::UserKey) {
                state = Menu;
                Features::requestRedraw();
                return true;
            }
        }
        return false;
    }

    void tick() {}

    void onExit() {
        if (state == Edit) state = View; // правки остаются в буфере до выхода из фичи
    }
}

REGISTER_FEATURE("Saved", "NFC", MwIcon::Folder, 30,
                 nullptr, input, tick, draw, onExit);
