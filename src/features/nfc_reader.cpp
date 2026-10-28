// =====================================================================
//  NFC: Reader — поднести карту, показать UID и тип (ISO14443A:
//  MIFARE Classic 1K/4K, Ultralight, NTAG21x, прочие Type-A).
//  Опрос поля из onTick с троттлингом; перерисовка — только при смене
//  карты (requestRedraw), не на каждый тик.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../../include/theme.h"

namespace {
    Nfc::Tag cur;
    bool haveCard = false;

    void uidToText(const Nfc::Tag& tag, char* out, size_t n) {
        out[0] = 0;
        size_t pos = 0;
        for (uint8_t i = 0; i < tag.uidLen && pos + 3 < n; i++) {
            if (i && i % 2 == 0 && pos + 1 < n) out[pos++] = ' ';
            pos += snprintf(out + pos, n - pos, "%02X", tag.uid[i]);
        }
    }

    char atsText[40] = ""; // fingerprint: historic bytes от RATS (ISO14443-4)

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NFC READER");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (!Nfc::busPresent()) {
            c.setTextColor(Theme::ERR, Theme::BG_PANEL);
            c.drawString("PN532 not on bus (I2C 0x24)", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("module absent or wiring", x0 + 10, y0 + 42);
            return;
        }
        if (!Nfc::ensure()) {
            c.setTextColor(Theme::ERR, Theme::BG_PANEL);
            c.drawString("PN532 found, no response", x0 + 10, y0 + 26);
            return;
        }

        if (haveCard) {
            char uidText[40];
            uidToText(cur, uidText, sizeof(uidText));
            c.setTextColor(Theme::ACCENT_GLOW, Theme::BG_PANEL);
            c.drawString(uidText, x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
            c.drawString(cur.type, x0 + 10, y0 + 44);
            char line[48];
            if (cur.isClassic) {
                snprintf(line, sizeof(line), "Blocks: %d (1K/4K)", cur.blocks);
                c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
                c.drawString(line, x0 + 10, y0 + 62);
            } else if (cur.isNtag) {
                snprintf(line, sizeof(line), "Pages: %d (x4B)", cur.pages);
                c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
                c.drawString(line, x0 + 10, y0 + 62);
            }
            if (atsText[0]) {
                snprintf(line, sizeof(line), "ATS %s", atsText);
                c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
                c.drawString(line, x0 + 10, y0 + 78);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("swap cards / remove to re-read", x0 + 10, y0 + h - 14);
        } else {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG_PANEL);
            c.drawString("No card in field", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
            c.drawString("hold a card to the back", x0 + 10, y0 + 44);
        }
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        static uint32_t last = 0;
        if (millis() - last < 300) return; // опрос поля ~3 Гц
        last = millis();

        Nfc::Tag t;
        bool found = Nfc::readTag(t);
        bool same = haveCard && found && t.uidLen == cur.uidLen &&
                    memcmp(t.uid, cur.uid, t.uidLen) == 0;
        if (found != haveCard || (!same && found)) {
            if (found) {
                cur = t;
                // fingerprint: RATS у ISO14443-4 карт (ATS historic bytes)
                uint8_t ats[24];
                int atsLen = sizeof(ats);
                atsText[0] = 0;
                if (Nfc::rats(ats, &atsLen)) {
                    int pos = 0;
                    for (int i = 0; i < atsLen && pos + 2 < (int)sizeof(atsText); i++)
                        pos += snprintf(atsText + pos, sizeof(atsText) - pos, "%02X", ats[i]);
                }
            }
            haveCard = found;
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Reader", "NFC", MwIcon::Chip, 10,
                 nullptr, input, tick, draw, nullptr);
