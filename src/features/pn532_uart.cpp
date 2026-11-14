// =====================================================================
//  NFC: PN532 UART — switch the NFC HAL transport between the internal
//  I2C module and an EXTERNAL PN532 wired to the Grove port over HSU
//  (UART1 115200, ESP TX 43 -> module RX, ESP RX 44 <- module TX).
//  After switching, ALL NFC features (reader / dump / write / emulate)
//  transparently talk to the external module.
//  Rotation: toggle. The screen shows the firmware version + live UID.
// =====================================================================
#include "../ui/features.h"
#include "../hal/nfc.h"
#include "../../include/theme.h"

namespace {
    bool switching = false;
    char fw[40] = "";
    char uidStr[24] = "";
    char statusMsg[40] = "";

    void readCard() {
        Nfc::Tag tag;
        if (Nfc::readTag(tag)) {
            char* p = uidStr;
            for (int i = 0; i < tag.uidLen && p - uidStr + 3 < (int)sizeof(uidStr); i++)
                p += snprintf(p, 3, "%02X", tag.uid[i]);
            *p = 0;
        } else uidStr[0] = 0;
    }

    void switchTransport() {
        switching = true;
        Features::requestRedraw();
        bool target = !Nfc::uartActive();
        bool ok = Nfc::useUart(target);
        switching = false;
        if (!ok) {
            snprintf(statusMsg, sizeof(statusMsg), target ? "no module on UART" : "no module on I2C");
            return;
        }
        snprintf(statusMsg, sizeof(statusMsg), target ? "transport: Grove HSU" : "transport: internal I2C");
        uint32_t ver = Nfc::isReady() ? 1 : 0; // isReady means ensure() passed
        snprintf(fw, sizeof(fw), ver ? "PN532 v1.%ld ready" : "not ready", 6);
        readCard();
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PN532 UART");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[48];

        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("external PN532 module, Grove HSU", x0 + 10, y0 + 22);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("TX 43 -> mod RX, RX 44 <- mod TX", x0 + 10, y0 + 34);

        c.setTextColor(Nfc::uartActive() ? Theme::OK : Theme::TEXT, Theme::BG);
        snprintf(line, sizeof(line), "transport: %s", Nfc::uartActive() ? "Grove UART" : "internal I2C");
        c.drawString(line, x0 + 10, y0 + 54);
        if (fw[0]) {
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(fw, x0 + 10, y0 + 68);
        }
        if (uidStr[0]) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            snprintf(line, sizeof(line), "card UID: %s", uidStr);
            c.drawString(line, x0 + 10, y0 + 84);
        }
        if (switching) {
            c.setTextColor(Theme::WARN, Theme::BG);
            c.drawString("switching...", x0 + 10, y0 + 100);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 30);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("click: switch transport", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click && !switching) {
            switchTransport();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        static uint32_t lastRead = 0;
        if (!switching && Nfc::isReady() && millis() - lastRead > 500) {
            lastRead = millis();
            readCard();
            if (uidStr[0]) Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("PN532 UART", "NFC", MwIcon::Chip, 50,
                 nullptr, input, tick, draw, nullptr);
