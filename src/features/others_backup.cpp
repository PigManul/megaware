// =====================================================================
//  Others: Backup — настройки (NVS) <-> /backup/settings.txt (SD, иначе
//  Flash). Управление: клик — Save / Restore. ВНИМАНИЕ: Wi-Fi и web
//  пароли пишутся на SD открытым текстом — это бэкап для СВОЕЙ карты.
// =====================================================================
#include "../ui/features.h"
#include "../hal/config.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    const struct { const char* k; bool isInt; } CFG_KEYS[] = {
        {"ap_preset", true},    {"disp_bright", true}, {"disp_dim", true},
        {"theme_accent", true}, {"footer_hints", true},
        {"wifi_ssid", false},   {"wifi_pass", false},
        {"webuser", false},     {"webpass", false},
    };
    char statusMsg[52] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    fs::FS& backupFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void save() {
        backupFs().mkdir("/backup");
        File f = backupFs().open("/backup/settings.txt", FILE_WRITE);
        if (!f) { snprintf(statusMsg, sizeof(statusMsg), "save failed (no fs)"); statusUntil = millis() + 3000; statusOk = false; return; }
        for (auto& k : CFG_KEYS) {
            if (k.isInt) f.printf("%s=i:%d\n", k.k, Config::getInt(k.k, 0));
            else f.printf("%s=s:%s\n", k.k, Config::getString(k.k, "").c_str());
        }
        f.close();
        snprintf(statusMsg, sizeof(statusMsg), "saved /backup/settings.txt");
        statusUntil = millis() + 3000;
        statusOk = true;
    }

    void restore() {
        File f = backupFs().open("/backup/settings.txt", FILE_READ);
        if (!f) { snprintf(statusMsg, sizeof(statusMsg), "no /backup/settings.txt"); statusUntil = millis() + 3000; statusOk = false; return; }
        int applied = 0;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String key = line.substring(0, eq);
            String val = line.substring(eq + 1);
            if (val.startsWith("i:")) Config::setInt(key.c_str(), val.substring(2).toInt());
            else if (val.startsWith("s:")) Config::setString(key.c_str(), val.substring(2));
            else continue;
            applied++;
        }
        f.close();
        snprintf(statusMsg, sizeof(statusMsg), "restored %d — reboot to apply", applied);
        statusUntil = millis() + 3000;
        statusOk = true;
    }

    int selIndex = 0; // 0 = Save, 1 = Restore

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "BACKUP");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        bool sel0 = (selIndex % 2 == 0);
        bool sel1 = !sel0;
        if (sel0) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y0 + 30); }
        if (sel1) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y0 + 48); }
        c.setTextDatum(textdatum_t::middle_left);
        c.setTextColor(sel0 ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
        c.drawString("Save settings -> /backup/", x0 + 12, y0 + 30);
        c.setTextColor(sel1 ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
        c.drawString("Restore settings", x0 + 12, y0 + 48);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::ERR, Theme::BG);
        c.drawString("Wi-Fi/web passwords go to SD!", x0 + 10, y0 + 66);
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + 84);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("file: /backup/settings.txt", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            selIndex += in.rotationSteps;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (selIndex % 2 == 0) save();
            else restore();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Backup", "Others", MwIcon::Others, 5,
                 nullptr, input, tick, draw, nullptr);
