// =====================================================================
//  IR: Library — .ir files (/ir/*, Flipper format, also written by
//  Capture). TWO-LEVEL select: rotation picks a FILE, click opens it
//  and lists the signals inside (a .ir can hold many "name:" blocks,
//  e.g. POWER/Speed/Mode of a fan remote); rotation picks the signal,
//  click transmits. Hold OK deletes the file.
// =====================================================================
#include "../ui/features.h"
#include "../hal/ir.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    int selIndex = 0;
    char statusMsg[48] = "";
    uint32_t statusUntil = 0;

    // opened file + its signals
    char openName[64] = "";
    bool inSignals = false;
    Ir::IrSignal sigs[4];
    int sigCount = 0;
    int sigSel = 0;

    fs::FS& irFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, inSignals ? "IR SIGNALS" : "IR LIBRARY");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);

        if (!inSignals) {
            FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
            int n = Files::listCount(root, "/ir");
            int visible = (h - 24) / Theme::ROW_LIST;
            int selC = n ? (selIndex % n + n) % n : 0;
            int top = selC - visible + 1;
            if (top < 0) top = 0;
            for (int i = 0; i < visible && top + i < n; i++) {
                FsEntry e = Files::listEntry(root, "/ir", top + i);
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
                c.drawString("IR Capture saves to /ir/", x0 + 12, y0 + 38);
            } else {
                c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
                c.drawString("click: open   hold: delete", x0 + 12, y0 + h - 4);
            }
        } else {
            // signal list inside the opened file
            for (int i = 0; i < sigCount; i++) {
                bool s = (i == sigSel);
                int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
                if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
                c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
                c.drawString(sigs[i].name, x0 + 12, y);
            }
            c.setTextDatum(textdatum_t::top_left);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            char line[40];
            const Ir::IrSignal& s = sigs[sigSel];
            if (s.parsed)
                snprintf(line, sizeof(line), "%s %08lX", s.proto, (unsigned long)s.command);
            else
                snprintf(line, sizeof(line), "raw %d pts  %lu Hz", s.rawLen, (unsigned long)s.freq);
            c.drawString(line, x0 + 10, y0 + h - 30);
            c.drawString("rot: signal  click: TX  quit: back", x0 + 10, y0 + h - 14);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 18);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/ir");

        if (!inSignals) {
            if (in.rotationSteps != 0 && n > 0) {
                selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::Click && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, "/ir", selC);
                snprintf(openName, sizeof(openName), "%s", e.name);
                sigCount = Ir::loadSignals(String("/ir/") + e.name, sigs, 8);
                sigSel = 0;
                if (sigCount <= 0) {
                    snprintf(statusMsg, sizeof(statusMsg), "parse failed");
                    statusUntil = millis() + 2500;
                } else {
                    inSignals = true;
                }
                Features::requestRedraw();
                return true;
            }
            if (in.event == InputEvent::LongPress && n > 0) {
                int selC = (selIndex % n + n) % n;
                FsEntry e = Files::listEntry(root, "/ir", selC);
                if (irFs().remove(String("/ir/") + e.name)) {
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

        // signal level
        if (in.rotationSteps != 0 && sigCount > 0) {
            sigSel = ((sigSel + in.rotationSteps) % sigCount + sigCount) % sigCount;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && sigCount > 0) {
            bool ok = Ir::sendSignal(sigs[sigSel]);
            snprintf(statusMsg, sizeof(statusMsg), "%s: %s", sigs[sigSel].name, ok ? "TX OK" : "TX failed");
            statusUntil = millis() + 2500;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Library", "IR", MwIcon::Folder, 30,
                 nullptr, input, tick, draw, nullptr);
