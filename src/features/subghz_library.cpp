// =====================================================================
//  Sub-GHz: Library — библиотека сохранённых сигналов (/subghz/*.sub,
//  их пишет Capture). Управление: вращение — выбор, клик — ПЕРЕДАТЬ,
//  hold OK — удалить. Передача блокирует на время сигнала (1-2 c).
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    int selIndex = 0;
    char statusMsg[48] = "";
    uint32_t statusUntil = 0;

    fs::FS& libFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "SIGNAL LIBRARY");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);

        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/subghz");
        int visible = (h - 24) / Theme::ROW_LIST;
        int selC = n ? (selIndex % n + n) % n : 0;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            FsEntry e = Files::listEntry(root, "/subghz", top + i);
            bool s = (top + i == selC);
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("library empty", x0 + 12, y0 + 20);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Capture saves to /subghz/", x0 + 12, y0 + 38);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: transmit   hold: delete", x0 + 12, y0 + h - 4);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 18);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/subghz");

        if (in.rotationSteps != 0 && n > 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/subghz", selC);
            bool ok = SubGhz::transmitFile(String("/subghz/") + e.name);
            snprintf(statusMsg, sizeof(statusMsg), "%s: %s", e.name, ok ? "TX OK" : "TX failed");
            statusUntil = millis() + 2500;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/subghz", selC);
            if (libFs().remove(String("/subghz/") + e.name)) {
                snprintf(statusMsg, sizeof(statusMsg), "deleted: %s", e.name);
                Files::invalidateCache();
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "delete FAILED");
            }
            statusUntil = millis() + 2500;
            if (selC >= n - 1) selIndex -= 1;
            if (selIndex < 0) selIndex = 0;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Library", MwCat::SubGhz, MwIcon::Folder, 40,
                 nullptr, input, tick, draw, nullptr);
