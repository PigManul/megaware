// =====================================================================
//  NFC: NTAG — парольная защита СВОИХ NTAG21x/Ultralight:
//  Set: PWD=FFFFFFFF, PACK=0000, AUTH0 = первая страница данных
//       (запись/чтение с этой страницы — по паролю);
//  Clear: AUTH0 = 0xFF (защита снята).
//  Страницы CFG/PWD/PACK вычисляются из объёма метки (N-4..N-1).
//  ВНИМАНИЕ: неверная запись CFG может сделать метку неудобной —
//  пароль известен (FFFFFFFF), защита снимается этой же фичей.
//  Управление: вращение — Set/Clear, клик — применить к карте в поле.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../../include/theme.h"

namespace {
    enum Action : uint8_t { ActSet, ActClear };
    uint8_t action = ActSet;
    char statusMsg[52] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    void apply(bool set) {
        Nfc::Tag tag;
        if (!Nfc::readTag(tag)) { setStatus("no card in field", false); return; }
        if (!tag.isNtag) { setStatus("not NTAG/UL", false); return; }

        int pages = tag.pages;
        int pageCfg1 = pages - 3, pagePwd = pages - 2, pagePack = pages - 1;

        if (set) {
            // CFG1: [RFUI, MIRROR..., AUTH0] -> AUTH0 = 0x04 (первая страница
            // данных); биты зеркала сохраняем из текущего CFG1
            uint8_t cfg1buf[4] = {0x00, 0x00, 0x00, 0x04};
            uint8_t cur[4];
            if (Nfc::ntagReadPage(pageCfg1, cur)) { cfg1buf[1] = cur[1]; cfg1buf[2] = cur[2]; }
            if (!Nfc::ntagWritePage(pageCfg1, cfg1buf)) { setStatus("CFG1 write failed", false); return; }
            uint8_t pw[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            uint8_t pk[4] = {0x00, 0x00, 0x00, 0x00};
            if (!Nfc::ntagWritePage(pagePwd, pw) || !Nfc::ntagWritePage(pagePack, pk)) {
                setStatus("PWD/PACK write failed", false);
                return;
            }
            setStatus("protected (pwd FFFFFFFF)", true);
        } else {
            uint8_t off[4] = {0x00, 0x00, 0x00, 0xFF}; // AUTH0 = 0xFF
            if (!Nfc::ntagWritePage(pageCfg1, off)) { setStatus("CFG1 write failed", false); return; }
            setStatus("protection cleared", true);
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NTAG TOOLS");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (!Nfc::busPresent()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("PN532 not on bus (I2C 0x24)", x0 + 10, y0 + 26);
            return;
        }
        if (!Nfc::ensure()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("PN532 found, no response", x0 + 10, y0 + 26);
            return;
        }

        const char* items[2] = {"Set password", "Clear protection"};
        c.setTextDatum(textdatum_t::middle_left);
        for (int i = 0; i < 2; i++) {
            int y = y0 + 26 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            bool sel = (action == i);
            if (sel) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(sel ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(items[i], x0 + 12, y);
        }
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("hold card, click = apply", x0 + 10, y0 + h - 28);
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextDatum(textdatum_t::middle_left);
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 12, y0 + h - 14);
        }
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            action = (action + in.rotationSteps) % 2;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            apply(action == ActSet);
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("NTAG", "NFC", MwIcon::Chip, 50,
                 nullptr, input, tick, draw, nullptr);
