// =====================================================================
//  IR: Capture — захват сигнала с пульта (декод + RAW-тайминги),
//  реплей и сохранение в /ir/cap_<ms>.ir (Flipper-формат, совместим
//  с библиотекой и Flipper Zero). Управление:
//    клик — встать на приём / (после захвата) повторить,
//    hold — сохранить захваченное в файл.
// =====================================================================
#include "../ui/features.h"
#include "../hal/ir.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { Idle, Armed, Captured };
    State state = Idle;
    Ir::Packet cap;
    bool decoded = false; // пакет декодировался в известный протокол
    char capName[40] = "";
    char statusMsg[40] = "";
    uint32_t statusUntil = 0;

    fs::FS& irFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void saveCap() {
        irFs().mkdir("/ir");
        snprintf(capName, sizeof(capName), "/ir/cap_%lu.ir", (unsigned long)millis());
        File f = irFs().open(capName, FILE_WRITE);
        if (!f) { snprintf(statusMsg, sizeof(statusMsg), "save failed"); statusUntil = millis() + 2500; return; }
        f.println("Filetype: Flipper IR file");
        f.println("Version: 1");
        f.println("#");
        f.printf("name: cap_%lu\n", (unsigned long)millis());
        if (decoded) {
            f.println("type: parsed");
            f.printf("protocol: %s\n", cap.proto);
            f.printf("address: %02X %02X %02X %02X\n",
                     (unsigned)((cap.address >> 24) & 0xFF), (unsigned)((cap.address >> 16) & 0xFF),
                     (unsigned)((cap.address >> 8) & 0xFF), (unsigned)(cap.address & 0xFF));
            f.printf("command: %02X %02X %02X %02X\n",
                     (unsigned)((cap.command >> 24) & 0xFF), (unsigned)((cap.command >> 16) & 0xFF),
                     (unsigned)((cap.command >> 8) & 0xFF), (unsigned)(cap.command & 0xFF));
        } else {
            f.println("type: raw");
            f.printf("frequency: %u\n", (unsigned)cap.rawFreq);
            f.println("duty_cycle: 0.330000");
            f.print("data: ");
            for (int i = 0; i < cap.rawLen; i++) {
                long v = cap.raw[i];
                f.print(i % 2 == 0 ? v : -v);
                f.print(' ');
            }
            f.println();
        }
        f.close();
        Files::invalidateCache();
        snprintf(statusMsg, sizeof(statusMsg), "saved %s", capName);
        statusUntil = millis() + 2500;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "IR CAPTURE");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (state == Idle) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("click: arm receiver", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("RX: GPIO1  TX: GPIO2", x0 + 10, y0 + 44);
        } else if (state == Armed) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("LISTENING...", x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("press the remote button", x0 + 10, y0 + 44);
        } else {
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(decoded ? "DECODED SIGNAL" : "RAW SIGNAL", x0 + 10, y0 + 24);
            char line[56];
            snprintf(line, sizeof(line), "%s  %u bits  %d edges",
                     cap.proto, cap.bits, cap.rawLen);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 42);
            snprintf(line, sizeof(line), "value 0x%llX", (unsigned long long)cap.value);
            c.drawString(line, x0 + 10, y0 + 58);
            if (statusMsg[0] && millis() < statusUntil) {
                c.setTextColor(Theme::OK, Theme::BG);
                c.drawString(statusMsg, x0 + 10, y0 + 74);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: replay   hold: save .ir", x0 + 10, y0 + h - 14);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            if (state == Idle) {
                state = Armed;
                Ir::begin();
            } else if (state == Armed) {
                state = Idle; // снять приём
            } else {
                // реплей: raw всегда доступен
                Ir::sendRaw(cap.rawFreq, cap.raw, cap.rawLen);
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress && state == Captured) {
            saveCap();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (state != Armed) return;
        Ir::Packet pkt;
        if (Ir::poll(pkt)) {
            cap = pkt;
            decoded = strcmp(pkt.proto, "UNKNOWN") != 0 && pkt.proto[0] != 0;
            state = Captured;
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("Capture", "IR", MwIcon::Antenna, 20,
                 nullptr, input, tick, draw, nullptr);
