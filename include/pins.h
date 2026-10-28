#pragma once
// =====================================================================
//  Распиновка LilyGo T-Embed CC1101 (hardware v1.0)
//  Подтверждено pins_arduino.h из прошивки Bruce для этой же платы —
//  НЕ МЕНЯЙ эти значения не глядя в схему/лог реальной сборки.
// =====================================================================

#define PIN_BOARD_PWR_EN   15
#define PIN_USER_KEY        6

#define PIN_WS2812_DATA    14
#define WS2812_NUM_LEDS     8

#define DISPLAY_WIDTH      320
#define DISPLAY_HEIGHT     170

#define PIN_SPI_SCK        11
#define PIN_SPI_MOSI        9
#define PIN_SPI_MISO       10

// --- Дисплей ST7789V — на ОБЩЕЙ SPI-шине с CC1101/SD (SCK/MOSI/MISO выше),
//     отдельные у него только DC/CS/RST/BL. См. src/ui/lgfx_config.h. ---
#define PIN_TFT_DC         16
#define PIN_TFT_CS         41
#define PIN_TFT_RST        40
#define PIN_TFT_BL         21

#define PIN_SD_CS          13

#define PIN_CC1101_CS       12
#define PIN_CC1101_GDO0      3
#define PIN_CC1101_GDO2     38
#define PIN_CC1101_SW1      47
#define PIN_CC1101_SW0      48

#define PIN_ENCODER_A        4
#define PIN_ENCODER_B        5
#define PIN_ENCODER_KEY      0

#define PIN_I2C_SDA          8
#define PIN_I2C_SCL         18
#define I2C_ADDR_PN532    0x24
#define I2C_ADDR_BQ27220  0x55
#define I2C_ADDR_BQ25896  0x6B

// --- IR (подтверждено: Bruce pins_arduino.h + LilyGo README) ---
#define PIN_IR_TX            2
#define PIN_IR_RX            1

// --- Микрофон I2S PDM ---
#define PIN_MIC_CLK         39
#define PIN_MIC_DATA        42

// --- Динамик NS4168 (пока не используется; 46/45 — страповые,
//     дёргать можно только после старта) ---
#define PIN_SPK_BCLK        46
#define PIN_SPK_LRCLK       40
#define PIN_SPK_DOUT         7

// --- PN532 управление (то же железо, что I2C-адрес 0x24) ---
#define PIN_PN532_RST       45
#define PIN_PN532_IRQ       17

// --- NRF24 / W5500 — внешние модули в Grove-порт, общая SPI (9/10/11) ---
#define PIN_NRF_CE          43
#define PIN_NRF_CS          44
#define PIN_W5500_CS        44
#define PIN_W5500_IRQ       43
