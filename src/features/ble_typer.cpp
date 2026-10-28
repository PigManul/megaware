// =====================================================================
//  Bluetooth: BLE Typer — устройство работает BLE-клавиатурой для твоего
//  телефона/ПК. Текст: файлы /typers/*.txt (заливаются через WebUI)
//  ИЛИ набранный вручную на устройстве (hold в списке — композер:
//  вращение — выбор символа, клик — дописать, hold — backspace,
//  Quit — отправить набранное).
//
//  Надёжность вывода (баг «выдаёт не то»): \r из CRLF-файлов
//  отбрасывается, \t -> KEY_TAB, не-ASCII байты UTF-8 пропускаются
//  (HID-клавиатура шлёт раскладочные keycode, юникод ей не передать),
//  пауза между символами 12 мс.
// =====================================================================
#include "../ui/features.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <BleKeyboard.h>
#include <BLEDevice.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    BleKeyboard kb("MEGAWARE", "LilyGo", 100);
    bool bleStarted = false;

    enum State : uint8_t { List, WaitHost, Typing, Compose };
    State state = List;
    int selIndex = 0;
    String typeBuf;                // текст к отправке (из файла или композера)
    size_t typed = 0;
    size_t total = 0;
    char typeName[40] = "";
    char statusMsg[40] = "";

    // композер
    const char* const CHARSET[] = {
        "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
        "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
        "0","1","2","3","4","5","6","7","8","9",
        " ",".","-",",","!","?","@","#","_",":",";","/","\\","(",")","[","]","+","=","<LF>",
    };
    constexpr int CHARSET_N = sizeof(CHARSET) / sizeof(CHARSET[0]);
    int composeIdx = 0;
    String composeBuf;

    fs::FS& typerFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    // подготовка текста: \r отбрасываем, не-ASCII пропускаем
    String sanitize(const String& in) {
        String out;
        out.reserve(in.length());
        for (size_t i = 0; i < in.length(); i++) {
            uint8_t b = (uint8_t)in[i];
            if (b == '\r') continue;
            if (b >= 0x80) continue;
            out += (char)b;
        }
        return out;
    }

    void startType() {
        if (!bleStarted) { kb.begin(); bleStarted = true; }
        if (!kb.isConnected()) { state = WaitHost; return; }
        typed = 0;
        total = typeBuf.length();
        state = Typing;
    }

    void startTypeFile(const char* name) {
        String path = String("/typers/") + name;
        File f = typerFs().open(path, FILE_READ);
        if (!f) { snprintf(statusMsg, sizeof(statusMsg), "open failed"); return; }
        String text;
        while (f.available()) text += (char)f.read();
        f.close();
        typeBuf = sanitize(text);
        snprintf(typeName, sizeof(typeName), "%s", name);
        startType();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "BLE TYPER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (ESP.getFreeHeap() < 70000) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("low heap — release Wi-Fi RAM", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("(Settings -> Wi-Fi), Quit and reopen", x0 + 10, y0 + 40);
            return;
        }

        if (state == WaitHost) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("host not paired", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("pair 'MEGAWARE' in BT settings of", x0 + 10, y0 + 46);
            c.drawString("your host, then click again", x0 + 10, y0 + 62);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: retry   Quit: back", x0 + 10, y0 + h - 14);
            return;
        }
        if (state == Typing) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("TYPING...", x0 + 10, y0 + 26);
            char line[56];
            snprintf(line, sizeof(line), "%s  %u/%u chars", typeName,
                     (unsigned)typed, (unsigned)total);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 46);
            if (total > 0) c.fillRect(x0 + 10, y0 + h - 12, (int)((w - 20) * typed / total), 3, Theme::ACCENT);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Quit: abort", x0 + 10, y0 + h - 28);
            return;
        }
        if (state == Compose) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("COMPOSE", x0 + 10, y0 + 22);
            c.setFont(&fonts::Font4);
            c.setTextDatum(textdatum_t::middle_center);
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(strcmp(CHARSET[composeIdx], "<LF>") == 0 ? "\\n" : CHARSET[composeIdx],
                         x0 + w / 2, y0 + 52);
            c.setFont(&fonts::Font0);
            c.setTextSize(Theme::SIZE_TEXT);
            String tail = composeBuf;
            if (tail.length() > 34) tail = tail.substring(tail.length() - 34);
            c.setTextDatum(textdatum_t::middle_left);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(tail, x0 + 10, y0 + 88);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("rot: char  click: add  hold: del", x0 + 10, y0 + h - 28);
            c.drawString("Quit: type & send", x0 + 10, y0 + h - 14);
            return;
        }

        // список файлов + подсказка про композер
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/typers");
        int visible = (h - 40) / Theme::ROW_LIST;
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
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(statusMsg[0] ? statusMsg : "click: type file  hold: compose", x0 + 12, y0 + h - 4);
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        if (state == List) {
            int n = Files::listCount(root, "/typers");
            if (in.rotationSteps != 0 && n > 0) {
                selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, "/typers", selC);
                startTypeFile(e.name);
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress) {
                composeBuf = "";
                composeIdx = 0;
                state = Compose;
                Features::requestRedraw();
                return true;
            }
        } else if (state == WaitHost) {
            if (in.event == InputEvent::Click) {
                startType();
                Features::requestRedraw();
                return true;
            }
        } else if (state == Compose) {
            if (in.rotationSteps != 0) {
                composeIdx = ((composeIdx + in.rotationSteps) % CHARSET_N + CHARSET_N) % CHARSET_N;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click) {
                composeBuf += (strcmp(CHARSET[composeIdx], "<LF>") == 0) ? "\n" : CHARSET[composeIdx];
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress) {
                if (composeBuf.length()) composeBuf.remove(composeBuf.length() - 1);
                Features::requestRedraw();
                return true;
            }
        }
        return false;
    }

    void tick() {
        if (state != Typing) return;
        // ~10 символов за тик (по 12 мс) — хосты не теряют символы
        for (int i = 0; i < 10 && typed < total; i++) {
            char ch = typeBuf[typed];
            if (ch == '\n') kb.write(KEY_RETURN);
            else if (ch == '\t') kb.write(KEY_TAB);
            else kb.write(ch);
            typed++;
            delay(12);
        }
        Features::requestRedraw();
        if (typed >= total) {
            kb.releaseAll();
            state = List;
            snprintf(statusMsg, sizeof(statusMsg), "typed %s", typeName);
            BLEDevice::deinit(true); // печать окончена — вернуть heap
            bleStarted = false;
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (bleStarted) {
            kb.releaseAll();
            BLEDevice::deinit(true);
            bleStarted = false;
        }
        state = List;
    }
}

REGISTER_FEATURE("BLE Typer", "Others", MwIcon::Terminal, 30,
                 nullptr, input, tick, draw, onExit);
