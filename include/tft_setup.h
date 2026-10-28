#pragma once
// =====================================================================
//  User_Setup для TFT_eSPI — LilyGo T-Embed CC1101 (ST7789, 170x320)
//  Подключается через build_flags: -include include/tft_setup.h
// =====================================================================

#define USER_SETUP_ID 214

#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR   // если цвета инвертированы — поменяй на TFT_RGB

#define TFT_WIDTH   170
#define TFT_HEIGHT  320

// Панель этого модуля — IPS-матрица, ей обычно требуется инверсия цвета.
// Если картинка выглядит "негативом" — просто закомментируй строку ниже.
#define TFT_INVERSION_ON

// --- пины (см. include/pins.h — держим синхронизировано вручную,
//     TFT_eSPI не умеет #include сторонние заголовки в User_Setup) ---
#define TFT_MOSI  9
#define TFT_SCLK  11
#define TFT_CS    41
#define TFT_DC    16
#define TFT_RST   -1
#define TFT_BL    21
#define TFT_BACKLIGHT_ON HIGH

// На новых партиях T-Embed CC1101 дисплей НЕ инициализируется без явного
// SPI_MODE0 (подтверждённый баг платы, см. issue #43 в репозитории
// Xinyuan-LilyGO/T-Embed-CC1101 — "SPI Mode 0 is needed for display on
// models with newer display revisions"). Если экран не включается —
// в первую очередь проверяй, что эта строка присутствует.
#define TFT_SPI_MODE SPI_MODE0

// --- шрифты ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// --- скорость SPI ---
#define SPI_FREQUENCY        40000000
#define SPI_READ_FREQUENCY   16000000
#define SPI_TOUCH_FREQUENCY   2500000
