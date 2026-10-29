// =====================================================================
//  Tools: RNG — cryptographically strong random data (hardware TRNG).
//  Rotation — select format (HEX/Base64/Password) and length.
//  Click — generate. The data is shown on screen.
// =====================================================================
#include "../ui/features.h"
#include "../hal/trng.h"
#include "../../include/theme.h"

namespace {
    constexpr int FMT_N = 3;
    int fmt = 0; // 0=HEX 1=Base64 2=Password
    int length = 16;
    char output[129] = "";
    bool generated = false;
    int selField = 0; // 0=format, 1=length

    void generate() {
        if (fmt == 0) { // HEX
            uint8_t raw[64];
            Trng::rngBytes(raw, length);
            for (int i = 0; i < length; i++) snprintf(output + i * 2, 3, "%02X", raw[i]);
        } else if (fmt == 1) { // Base64 (random base64 characters)
            static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int pos = 0;
            for (int i = 0; i < length && pos < 128; i++) {
                uint32_t r = Trng::rng32();
                output[pos++] = B64[r % 64];
            }
            output[pos] = 0;
        } else { // Password
            static const char CS[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789!@#$%^&*";
            int csLen = strlen(CS);
            int pos = 0;
            for (int i = 0; i < length && pos < 128; i++) output[pos++] = CS[Trng::rng32() % csLen];
            output[pos] = 0;
        }
        generated = true;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "TRUE RNG");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);
        const char* fmtNames[3] = {"HEX", "Base64", "Password"};
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        char line[48];
        snprintf(line, sizeof(line), "format: %s (%s)", fmtNames[fmt], selField == 0 ? "rot" : "len");
        c.drawString(line, x0 + 10, y0 + 24);
        snprintf(line, sizeof(line), "length: %d bytes", length);
        c.drawString(line, x0 + 10, y0 + 40);
        if (generated) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            int row = 0;
            for (size_t i = 0; i < strlen(output) && row < 4; i += 22, row++) {
                char chunk[23];
                snprintf(chunk, sizeof(chunk), "%s", output + i);
                c.drawString(chunk, x0 + 10, y0 + 60 + row * Theme::ROW_LIST);
            }
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rot: fmt/len  click: gen", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            if (selField == 0) fmt = ((fmt + in.rotationSteps) % FMT_N + FMT_N) % FMT_N;
            else length = constrain(length + in.rotationSteps * 8, 8, 64);
            generated = false;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (selField == 0) { selField = 1; }
            else { generate(); selField = 0; }
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::LongPress) { selField = 0; generated = false; Features::requestRedraw(); return true; }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("RNG", "Tools", MwIcon::Screwdriver, 10,
                 nullptr, input, tick, draw, nullptr);
