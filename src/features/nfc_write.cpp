// =====================================================================
//  NFC: Write — NDEF на СВОИ NTAG21x/Ultralight: URL, Text, vCard.
//  Источник — файлы /ndef/*.txt (заливаются через WebUI):
//    URL:https://...    -> URI-запись
//    TEXT:hello world   -> Text-запись
//    VCARD:             -> text/vcard MIME, тело = весь файл ниже первой
//                          строки (BEGIN:VCARD...END:VCARD)
//  Управление: вращение — выбор файла, клик — поднести метку и записать
//  (CC + TLV), Quit — выход.
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

    fs::FS& ndefFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void setStatus(const char* msg, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", msg);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    // NDEF message for the record type into ndef[], returns length
    size_t buildNdef(const char* type, const char* payload, size_t payLen,
                     uint8_t* ndef, size_t cap) {
        size_t n = 0;
        if (strcmp(type, "URL") == 0) {
            // D1 01 <L> 55 <prefix 0x00> <url>
            size_t pl = 1 + payLen;
            if (5 + pl > cap) return 0;
            ndef[n++] = 0xD1; ndef[n++] = 0x01; ndef[n++] = (uint8_t)pl;
            ndef[n++] = 0x55; ndef[n++] = 0x00;
            memcpy(ndef + n, payload, payLen); n += payLen;
        } else if (strcmp(type, "TEXT") == 0) {
            // D1 01 <L> 54 02 "en" <text>
            size_t pl = 3 + payLen;
            if (4 + pl > cap) return 0;
            ndef[n++] = 0xD1; ndef[n++] = 0x01; ndef[n++] = (uint8_t)pl;
            ndef[n++] = 0x54; ndef[n++] = 0x02;
            ndef[n++] = 'e'; ndef[n++] = 'n';
            memcpy(ndef + n, payload, payLen); n += payLen;
        } else { // VCARD: MIME media record, type "text/vcard"
            static const char* MT = "text/vcard";
            size_t mtLen = strlen(MT);
            size_t pl = mtLen + payLen;
            if (4 + mtLen + payLen > cap) return 0;
            ndef[n++] = 0xD2; ndef[n++] = 0x01; ndef[n++] = (uint8_t)pl;
            memcpy(ndef + n, MT, mtLen); n += mtLen;
            memcpy(ndef + n, payload, payLen); n += payLen;
        }
        return n;
    }

    bool writeNdef(const char* type, const char* payload, size_t payLen) {
        Nfc::Tag tag;
        if (!Nfc::readTag(tag)) { setStatus("no card in field", false); return false; }
        if (!tag.isNtag) { setStatus("not NTAG/UL (Classic write unsupported)", false); return false; }

        uint8_t ndef[512];
        size_t ndefLen = buildNdef(type, payload, payLen, ndef, sizeof(ndef));
        if (ndefLen == 0) { setStatus("payload too big for record", false); return false; }

        // TLV: 03 <len> <ndef> FE (long TLV when > 254)
        uint8_t tlv[516];
        size_t tlvLen = 0;
        tlv[tlvLen++] = 0x03;
        if (ndefLen > 254) {
            tlv[tlvLen++] = 0xFF;
            tlv[tlvLen++] = (uint8_t)(ndefLen >> 8);
            tlv[tlvLen++] = (uint8_t)(ndefLen & 0xFF);
        } else tlv[tlvLen++] = (uint8_t)ndefLen;
        memcpy(tlv + tlvLen, ndef, ndefLen);
        tlvLen += ndefLen;
        tlv[tlvLen++] = 0xFE;

        // CC (page 3): keep access bytes, mark NDEF-compatible
        uint8_t cc[4];
        if (!Nfc::ntagReadPage(3, cc)) { setStatus("CC read failed", false); return false; }
        cc[0] = 0xE1; cc[1] = 0x10;
        if (!Nfc::ntagWritePage(3, cc)) { setStatus("CC write failed", false); return false; }

        size_t off = 0;
        uint8_t page = 4;
        while (off < tlvLen) {
            uint8_t chunk[4] = {0, 0, 0, 0};
            size_t n = tlvLen - off;
            if (n > 4) n = 4;
            memcpy(chunk, tlv + off, n);
            if (!Nfc::ntagWritePage(page, chunk)) { setStatus("page write failed", false); return false; }
            off += n;
            page++;
            if (page >= tag.pages) { setStatus("tag too small", false); return false; }
        }

        uint8_t back[4];
        if (Nfc::ntagReadPage(4, back) && memcmp(back, tlv, 4) == 0) {
            setStatus("written OK", true);
            return true;
        }
        setStatus("verify FAILED", false);
        return false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NFC WRITE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);

        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/ndef");
        int visible = (h - 24) / Theme::ROW_LIST;
        int selC = n ? (selIndex % n + n) % n : 0;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            FsEntry e = Files::listEntry(root, "/ndef", top + i);
            bool s = (top + i == selC);
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no ndef files", x0 + 12, y0 + 20);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("upload /ndef/*.txt:", x0 + 12, y0 + 38);
            c.drawString("URL:https://... | TEXT:hello", x0 + 12, y0 + 48);
            c.drawString("VCARD: body below line 1", x0 + 12, y0 + 58);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: hold card & write", x0 + 12, y0 + h - 4);
        }
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 18);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/ndef");
        if (in.rotationSteps != 0 && n > 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/ndef", selC);
            File f = ndefFs().open(String("/ndef/") + e.name, FILE_READ);
            if (!f) { setStatus("open failed", false); Features::requestRedraw(); return true; }
            String content = f.readStringUntil('\n');
            // vCard: body = everything below the first line
            String rest = f.readStringUntil(0);
            f.close();
            content.trim();
            const char* type = "URL";
            String payload;
            if (content.startsWith("URL:")) { type = "URL"; payload = content.substring(4); }
            else if (content.startsWith("TEXT:")) { type = "TEXT"; payload = content.substring(5); }
            else if (content.startsWith("VCARD:")) {
                type = "VCARD";
                rest.trim();
                payload = rest;
            } else payload = content; // bare line = URL, backwards compat
            if (payload.length() > 450) payload = payload.substring(0, 450);
            writeNdef(type, payload.c_str(), payload.length());
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Write", "NFC", MwIcon::Chip, 40,
                 nullptr, input, tick, draw, nullptr);
