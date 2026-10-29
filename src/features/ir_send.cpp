// =====================================================================
//  IR: Send — ручная передача ИК-команд + база ТВ power-кодов.
//  Управление: вращение — выбор, клик — подтвердить / отправить,
//  hold — назад. Протокол → address → command → SEND.
//  ТВ-коды: вращение листает бренды, клик отправляет power.
// =====================================================================
#include "../ui/features.h"
#include "../hal/ir.h"
#include "../../include/theme.h"

namespace {
    enum Stage : uint8_t { Proto, Addr, Cmd, Result };
    Stage stage = Proto;
    int protoIdx = 0;
    uint32_t addr = 0;
    uint32_t cmd = 0;
    char statusMsg[40] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    struct TvCode { const char* brand; int proto; uint32_t value; };
    const TvCode TV_CODES[] = {
        {"Samsung",   0, 0x0707E01F},
        {"LG",        0, 0xFE01FE01},
        {"Sony",      1, 0x0000A90},
        {"Panasonic", 6, 0xBCBD},
        {"Philips",   2, 0x0000000C},
        {"Toshiba",   0, 0xFF02FD02},
        {"Sharp",     0, 0x02FDC23D},
        {"Vizio",     0, 0x04FBE01F},
    };
    constexpr int TV_N = sizeof(TV_CODES) / sizeof(TV_CODES[0]);
    int tvIdx = 0;

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 3000;
        statusOk = ok;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "IR SEND");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);

        if (stage == Result) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("hold: back to protocol", x0 + 10, y0 + h - 14);
            return;
        }

        if (stage == Proto) {
            // список протоколов
            c.setTextDatum(textdatum_t::middle_left);
            for (int i = 0; i < Ir::protocolCount(); i++) {
                int y = y0 + 40 + i * Theme::ROW_LIST;
                bool sel = (i == protoIdx);
                if (sel) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
                c.setTextColor(sel ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
                c.drawString(Ir::protocolName(i), x0 + 12, y);
            }
            // ТВ-коды справа снизу
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("TV:", x0 + w - 60, y0 + 40);
            c.drawString(TV_CODES[tvIdx].brand, x0 + w - 60, y0 + 56);
            c.drawString("rot: browse", x0 + w - 60, y0 + 72);
            c.setTextDatum(textdatum_t::top_left);
            return;
        }

        // Addr / Cmd поля
        char buf[24];
        int fg = (stage == Addr) ? 0 : 1;
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(Ir::protocolName(protoIdx), x0 + 10, y0 + 22);

        snprintf(buf, sizeof(buf), "addr: 0x%04lX", (unsigned long)addr);
        c.setTextColor(stage == Addr ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
        c.drawString(buf, x0 + 10, y0 + 44);
        snprintf(buf, sizeof(buf), "cmd:  0x%04lX", (unsigned long)cmd);
        c.setTextColor(stage == Cmd ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
        c.drawString(buf, x0 + 10, y0 + 60);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        if (stage == Addr) c.drawString("rot: ±1  click: next  hold: back", x0 + 10, y0 + h - 14);
        else c.drawString("rot: ±1  click: SEND  hold: back", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            if (stage == Proto) {
                // протоколы: левая половина вращения, ТВ-коды: правая
                if (in.rotationSteps > 10 || in.rotationSteps < -10) {
                    // большое вращение — ТВ-коды
                    tvIdx = (tvIdx + in.rotationSteps / 10) % TV_N;
                    if (tvIdx < 0) tvIdx += TV_N;
                } else {
                    protoIdx = (protoIdx + in.rotationSteps) % Ir::protocolCount();
                    if (protoIdx < 0) protoIdx += Ir::protocolCount();
                }
            } else if (stage == Addr) {
                addr = (uint32_t)constrain((long)addr + in.rotationSteps, 0L, 65535L);
            } else if (stage == Cmd) {
                cmd = (uint32_t)constrain((long)cmd + in.rotationSteps, 0L, 65535L);
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (stage == Proto) {
                stage = Addr;
                addr = 0;
                cmd = 0;
            } else if (stage == Addr) {
                stage = Cmd;
            } else if (stage == Cmd) {
                bool ok = Ir::sendByIndex(protoIdx, addr, cmd);
                setStatus(ok ? "IR sent!" : "send failed", ok);
                stage = Result;
            } else { // Result
                stage = Proto;
            }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) {
            if (stage == Cmd) stage = Addr;
            else if (stage == Addr) stage = Proto;
            else if (stage == Result) stage = Proto;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}

    // отправка ТВ-кода: hold в Proto-стадии
    void onHold() {
        auto& tc = TV_CODES[tvIdx];
        Ir::sendByIndex(tc.proto, tc.value >> 16, tc.value & 0xFFFF);
        setStatus(tc.brand, true);
    }
}

REGISTER_FEATURE("Send", "IR", MwIcon::Chip, 40,
                 nullptr, input, tick, draw, nullptr);
