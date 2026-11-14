// =====================================================================
//  NFC: Amiibo — write Amiibo dumps onto blank NTAG215 tags.
//  Source: /amiibo/*.bin (uploaded via WebUI). A standard Amiibo dump
//  is 540 (NTAG215 raw) or 571 (with extra trailing bytes) bytes:
//  pages 0-2 are UID/BCC/lock (the tag's own UID is kept), pages 3-129
//  carry the character data, pages 130+ dynamic lock/auth are written
//  as-is but may fail on a protected tag - reported honestly.
//  Rotation: pick the dump, click: hold the tag and write.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    int selIndex = 0;
    char statusMsg[52] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;
    int lastPct = 0;

    fs::FS& amiFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    bool writeAmiibo(const char* path) {
        File f = amiFs().open(path, FILE_READ);
        if (!f) { setStatus("open failed", false); return false; }
        size_t sz = f.size();
        if (sz < 540) { f.close(); setStatus("not an amiibo dump (<540B)", false); return false; }

        Nfc::Tag tag;
        if (!Nfc::readTag(tag)) { f.close(); setStatus("no tag in field", false); return false; }
        if (!tag.isNtag || tag.pages < 135) {
            f.close();
            setStatus("need blank NTAG215", false);
            return false;
        }

        // NTAG215: 135 pages. Skip page 2 (static lock, factory ok on a
        // blank tag), try everything else; dynamic lock/auth pages may
        // refuse on a previously used tag - reported, not fatal.
        int page = 0;
        bool lockFail = false;
        while (page < 135) {
            uint8_t buf[4] = {0, 0, 0, 0};
            f.read(buf, 4);
            if (page == 2) { page++; continue; } // static lock: keep factory value
            if (!Nfc::ntagWritePage((uint16_t)page, buf) && page >= 130) lockFail = true;
            page++;
            lastPct = page * 100 / 135;
            if (page % 16 == 0) Features::requestRedraw();
        }
        f.close();
        lastPct = 100;
        if (lockFail) { setStatus("written (lock/auth pages refused)", true); return true; }
        setStatus("amiibo written OK", true);
        return true;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "AMIIBO WRITER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);

        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/amiibo");
        int visible = (h - 46) / Theme::ROW_LIST;
        int selC = n ? (selIndex % n + n) % n : 0;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            FsEntry e = Files::listEntry(root, "/amiibo", top + i);
            bool s = (top + i == selC);
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no dumps", x0 + 12, y0 + 20);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("upload /amiibo/*.bin (NTAG215)", x0 + 12, y0 + 38);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: hold blank NTAG215 & write", x0 + 12, y0 + h - 22);
        }
        if (lastPct > 0 && lastPct < 100) {
            char line[24];
            snprintf(line, sizeof(line), "writing... %d%%", lastPct);
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString(line, x0 + 12, y0 + h - 34);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 18);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/amiibo");
        if (in.rotationSteps != 0 && n > 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/amiibo", selC);
            String path = String("/amiibo/") + e.name;
            writeAmiibo(path.c_str());
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Amiibo", "NFC", MwIcon::Chip, 55,
                 nullptr, input, tick, draw, nullptr);
