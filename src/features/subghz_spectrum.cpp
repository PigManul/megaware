// =====================================================================
//  Sub-GHz: Spectrum — спектроанализатор эфира. Вращение переключает
//  диапазон-пресет (433 / 868 / 315 / 915). Пока фича открыта, идёт
//  непрерывная развёртка (~50 мс на проход); данные live-текут и в
//  WebUI: /terminal рисует их в реальном времени (/api/spectrum).
// =====================================================================
#include "../ui/features.h"
#include "../radio/subghz.h"
#include "../../include/theme.h"

namespace {
    int band = 0;
    uint32_t lastSweep = 0;
    bool radioRetuned = false;

    const struct { const char* name; float start, step; int count; } BANDS[4] = {
        {"433 MHz", 433.00f, 0.05f, 36},
        {"868 MHz", 868.00f, 0.025f, 25},
        {"315 MHz", 314.00f, 0.05f, 41},
        {"915 MHz", 902.00f, 0.20f, 66},
    };

    void applyBand() {
        SubGhz::setSpectrumRange(BANDS[band].start, BANDS[band].step, BANDS[band].count);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "SPECTRUM");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);

        if (!SubGhz::isReady()) {
            c.setTextDatum(textdatum_t::top_left);
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("CC1101 not found", x0 + 10, y0 + 24);
            return;
        }

        SubGhz::SpectrumData& s = SubGhz::sharedSpectrum();
        if (s.count <= 0) {
            applyBand();
            return;
        }

        int baseY = y0 + h - 18;
        int bw = (w - 8) / s.count;
        if (bw < 2) bw = 2;
        // шкала: -100..-40 дБм -> высота
        c.setTextDatum(textdatum_t::middle_center);
        float peak = -999;
        int pi = 0;
        for (int i = 0; i < s.count; i++) {
            float v = (s.dbm[i] + 100.0f) / 60.0f;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            int bh = (int)(v * (h - 44));
            c.fillRect(x0 + 4 + i * bw, baseY - bh, bw - 1, bh, Theme::ACCENT);
            if (s.dbm[i] > peak) { peak = s.dbm[i]; pi = i; }
        }
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString(BANDS[band].name, x0 + 30, baseY + 8);
        char pk[32];
        snprintf(pk, sizeof(pk), "peak %.0f dBm @ %.2f", peak, s.startMHz + s.stepMHz * pi);
        c.setTextColor(Theme::TEXT, Theme::BG);
        c.drawString(pk, x0 + w - 8, baseY + 8);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rotate: band   (live in WebUI /terminal)", x0 + 10, y0 + 22);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            band = ((band + in.rotationSteps) % 4 + 4) % 4;
            applyBand();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        // развёртка не чаще раза в 60 мс — радио не душит остальное
        if (millis() - lastSweep >= 60) {
            lastSweep = millis();
            SubGhz::sweepOnce();
            Features::requestRedraw();
        }
    }

    void onEnter() {
        radioRetuned = false;
        applyBand();
    }

    void onExit() {
        // вернуть радио на «пользовательскую» частоту
        SubGhz::setFrequency(SubGhz::currentFrequencyMHz());
    }
}

REGISTER_FEATURE("Spectrum", MwCat::SubGhz, MwIcon::Pulse, 30,
                 onEnter, input, tick, draw, onExit);
