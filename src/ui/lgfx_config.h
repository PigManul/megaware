#pragma once
#include <LovyanGFX.hpp>
#include "../../include/pins.h"

// =====================================================================
//  Свой класс LGFX-панели для T-Embed CC1101 (вместо вендорской
//  обёртки LilyGo-display-library — та не даёт форсировать SPI_MODE0,
//  нужный для фикса issue #43 в репозитории платы, и не разделяет
//  memory_width/panel_width, что давало перекошенный экран).
// =====================================================================

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX(void) {
        { // --- шина SPI ---
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 0;           // фикс issue #43
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = false;
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = PIN_SPI_SCK;
            cfg.pin_mosi = PIN_SPI_MOSI;
            cfg.pin_miso = PIN_SPI_MISO;
            cfg.pin_dc   = PIN_TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        { // --- панель ST7789V: физически видно 170x320, но GRAM
          //     контроллера общая на 240x320 (тот же чип, что и у более
          //     широких панелей) ---
            auto cfg = _panel.config();
            cfg.pin_cs   = PIN_TFT_CS;
            cfg.pin_rst  = PIN_TFT_RST;
            cfg.pin_busy = -1;

            cfg.memory_width  = 240;
            cfg.memory_height = 320;
            cfg.panel_width   = 170;
            cfg.panel_height  = 320;

            cfg.offset_x = 35; // (240-170)/2 — центрируем видимые столбцы в GRAM
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.readable  = false;
            cfg.invert    = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel.config(cfg);
        }
        { // --- подсветка ---
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_TFT_BL;
            cfg.invert      = false;
            cfg.freq        = 5000;
            cfg.pwm_channel = 1;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};
