// =====================================================================
//  RF24: Scanner — активность 2.4 ГГц по каналам 0..126. НУЖЕН внешний
//  NRF24-модуль в Grove-порту (CE=43, CS=44, SPI 9/10/11). Шина вынесена
//  на отдельный контроллер (HSPI), чтобы не конфликтовать с дисплеем.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <SPI.h>
#include <RF24.h>

namespace {
    // ВАЖНО (баг "интерфейс замирает после входа в NRF24"): раньше здесь
    // создавался ОТДЕЛЬНЫЙ контроллер SPIClass(HSPI) и begin(11,10,9,44)
    // пере-маршрутизировал GPIO 9/10/11 на SPI3 — у дисплея (SPI2) их
    // выходные сигналы отбирались, экран замирал. Теперь тот же
    // контроллер SPI2, что у дисплея: операции RF24 короткие и идут
    // между кадрами, транзакции атомарны.
    RF24 radio(PIN_NRF_CE, PIN_NRF_CS, 1000000);
    bool radioOk = false;
    bool inited = false;

    uint8_t levels[126] = {0};    // активность по каналам (0..~5)
    uint32_t lastSweep = 0;

    bool initRadio() {
        if (radioOk) return true;
        if (!inited) {
            // ss НЕ передаём: CSN NRF24 дёргается через digitalWrite,
            // маршрутизация SS-сигнала на пин 44 сломала бы это
            SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
            inited = true;
        }
        radioOk = radio.begin(&SPI) && radio.isChipConnected();
        if (radioOk) radio.stopListening();
        return radioOk;
    }

    void sweep() {
        // 127 каналов по ~300 мкс = ~40 мс на проход
        radio.stopListening();
        for (int ch = 0; ch < 126; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(280);
            bool active = radio.testCarrier() || radio.testRPD();
            radio.stopListening();
            if (active) {
                if (levels[ch] < 250) levels[ch] += 8;   // подъём быстрый
            } else if (levels[ch]) {
                levels[ch] -= 1;                          // затухание медленное
            }
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "NRF24 SCANNER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!initRadio()) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString("NRF24 not detected", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("Grove: CE 43, CS 44, SPI 9/10/11", x0 + 10, y0 + 42);
            return;
        }

        int baseY = y0 + h - 18;
        int bw = (w - 8) / 126;
        if (bw < 2) bw = 2;
        c.setTextDatum(textdatum_t::middle_center);
        for (int ch = 0; ch < 126; ch++) {
            int bh = levels[ch] * (h - 46) / 250;
            if (bh > 0) c.fillRect(x0 + 4 + ch * bw, baseY - bh, bw - 1, bh, Theme::ACCENT);
        }
        c.setTextColor(Theme::TEXT_DIM, Theme::BG);
        c.drawString("2400", x0 + 10, baseY + 8);
        c.drawString("2525 MHz", x0 + w - 30, baseY + 8);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("external module (Grove port)", x0 + 10, y0 + 22);
    }

    bool input(const InputState& in) { return false; }

    void tick() {
        if (!radioOk && !initRadio()) return;
        if (millis() - lastSweep >= 80) {  // ~10 проходов/с
            lastSweep = millis();
            sweep();
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (radioOk) radio.powerDown();
    }
}

REGISTER_FEATURE("NRF Scan", "RF24", MwIcon::Nrf, 10,
                 nullptr, input, tick, draw, onExit);
