// =====================================================================
//  NFC: Write — запись NDEF-URL на СВОИ NTAG21x/Ultralight.
//  Источник URL — файлы /ndef/*.txt (заливаются через WebUI), первая
//  строка файла = полный URL. Управление: вращение — выбор файла,
//  клик — поднести метку и записать (CC + TLV + URI-запись), Quit — выход.
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

    bool writeNdefUrl(const char* url) {
        Nfc::Tag tag;
        if (!Nfc::readTag(tag)) { setStatus("no card in field", false); return false; }
        if (!tag.isNtag) { setStatus("not NTAG/UL (Classic write unsupported)", false); return false; }

        // NDEF URI-запись: D1 01 <L> 55 <prefix 0x00> <url>
        size_t urlLen = strlen(url);
        if (urlLen == 0 || urlLen > 100) { setStatus("empty/long URL", false); return false; }
        uint8_t ndef[104];
        size_t payloadLen = 1 + urlLen;
        size_t ndefLen = 0;
        ndef[ndefLen++] = 0xD1;
        ndef[ndefLen++] = 0x01;
        ndef[ndefLen++] = (uint8_t)payloadLen;
        ndef[ndefLen++] = 0x55; // тип "U"
        ndef[ndefLen++] = 0x00; // без префикса — URL целиком
        memcpy(ndef + ndefLen, url, urlLen);
        ndefLen += urlLen;

        // TLV: 03 <len> <ndef> FE
        uint8_t tlv[110];
        size_t tlvLen = 0;
        tlv[tlvLen++] = 0x03;
        tlv[tlvLen++] = (uint8_t)ndefLen;
        memcpy(tlv + tlvLen, ndef, ndefLen);
        tlvLen += ndefLen;
        tlv[tlvLen++] = 0xFE;

        // CC (стр. 3): читаем текущий, меняем только access-байты
        uint8_t cc[4];
        if (!Nfc::ntagReadPage(3, cc)) { setStatus("CC read failed", false); return false; }
        cc[0] = 0xE1; cc[1] = 0x10; // NDEF-совместимая разметка
        if (!Nfc::ntagWritePage(3, cc)) { setStatus("CC write failed", false); return false; }

        // данные по страницам с 4-й
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

        // верификация первой страницы данных
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
            c.drawString("no url files", x0 + 12, y0 + 20);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("upload /ndef/*.txt (line1 = URL)", x0 + 12, y0 + 38);
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
            String url = f.readStringUntil('\n');
            f.close();
            url.trim();
            writeNdefUrl(url.c_str());
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Write", "NFC", MwIcon::Chip, 40,
                 nullptr, input, tick, draw, nullptr);
