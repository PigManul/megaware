// =====================================================================
//  Tools: USB Typer — УСТРОЙСТВО РАБОТАЕТ USB-КЛАВИАТУРОЙ (TinyUSB HID).
//
//  СЕЙЧАС ВЫКЛЮЧЕНО: требует -DARDUINO_USB_MODE=0 (TinyUSB) в
//  platformio.ini, иначе HID не поднимется. В TinyUSB-режиме esptool не
//  умеет авто-ресет — прошивка по кабелю только через ручной boot-режим
//  (зажать кнопку энкодера = BOOT, щёлкнуть reset, отпустить) или через
//  WebUI -> UPDATE. Включение: MODE=0 здесь и в platformio.ini.
//
//  Текст — файлы /typers/*.txt (заливаются через WebUI). Вставь USB,
//  клик по файлу — печать; Quit — выход. Инструмент автоматизации
//  для СВОИХ машин.
// =====================================================================
#if 0

#include "../ui/features.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    USBHIDKeyboard kb;
    bool usbStarted = false;

    enum State : uint8_t { List, Typing };
    State state = List;
    int selIndex = 0;
    File typeFile;
    char typeName[40] = "";
    long typed = 0, total = 0;
    char statusMsg[40] = "";

    fs::FS& typerFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void ensureUsb() {
        if (usbStarted) return;
        kb.begin();
        USB.begin();
        usbStarted = true;
        delay(500); // дать хосту перечислить клавиатуру
    }

    void startType(const char* name) {
        ensureUsb();
        String path = String("/typers/") + name;
        typeFile = typerFs().open(path, FILE_READ);
        if (!typeFile) { snprintf(statusMsg, sizeof(statusMsg), "open failed"); return; }
        total = typeFile.size();
        typed = 0;
        snprintf(typeName, sizeof(typeName), "%s", name);
        state = Typing;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "USB TYPER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Typing) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("TYPING...", x0 + 10, y0 + 26);
            char line[56];
            snprintf(line, sizeof(line), "%s  %ld/%ld B", typeName, (long)typed, (long)total);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 46);
            if (total > 0) c.fillRect(x0 + 10, y0 + h - 12, (int)((w - 20) * typed / total), 3, Theme::ACCENT);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Quit: abort", x0 + 10, y0 + h - 28);
            return;
        }

        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/typers");
        int visible = (h - 24) / Theme::ROW_LIST;
        int selC = n ? (selIndex % n + n) % n : 0;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            FsEntry e = Files::listEntry(root, "/typers", top + i);
            bool s = (top + i == selC);
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no presets", x0 + 12, y0 + 20);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("upload /typers/*.txt via WebUI", x0 + 12, y0 + 38);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString(statusMsg[0] ? statusMsg : "click: type into host PC", x0 + 12, y0 + h - 4);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        if (state != List) return false;
        int n = Files::listCount(root, "/typers");
        if (in.rotationSteps != 0 && n > 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/typers", selC);
            startType(e.name);
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Typing) return;
        for (int i = 0; i < 12 && typed < total; i++) {
            int ch = typeFile.read();
            if (ch < 0) { typed = total; break; }
            if (ch == '\n') kb.write(KEY_RETURN);
            else kb.write((char)ch);
            typed++;
            delay(8);
        }
        Features::requestRedraw();
        if (typed >= total) {
            typeFile.close();
            kb.releaseAll();
            state = List;
            snprintf(statusMsg, sizeof(statusMsg), "typed %s", typeName);
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (typeFile) typeFile.close();
        if (usbStarted) kb.releaseAll();
        state = List;
    }
}

REGISTER_FEATURE("USB Typer", "Tools", MwIcon::Terminal, 10,
                 nullptr, input, tick, draw, onExit);

#endif // 0 — включение в шапке (нужен TinyUSB-режим)
