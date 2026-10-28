#include "../../include/theme.h"

// =====================================================================
//  Реализация акцентной группы темы: пресеты меняются на лету из
//  Settings -> Theme и переживают перезагрузку (NVS, см. Ui::begin).
//  TEXT_DIM выводится из акцента (~45% яркости), OK/ACCENT_* следуют.
// =====================================================================

namespace Theme {

uint16_t ACCENT      = RGB565(0x39, 0xFF, 0x88);
uint16_t TEXT_DIM    = RGB565(0x1A, 0x73, 0x3D);
uint16_t OK          = ACCENT;
uint16_t ACCENT_DIM  = TEXT_DIM;
uint16_t ACCENT_GLOW = ACCENT;

void setAccent(uint8_t preset) {
    uint8_t r, g, b;
    switch (preset % ACCENT_PRESETS) {
        case 1:  r = 0xFF; g = 0xB0; b = 0x20; break; // Amber
        case 2:  r = 0x2E; g = 0xD9; b = 0xFF; break; // Cyan
        case 3:  r = 0xE8; g = 0xE8; b = 0xE8; break; // White
        default: r = 0x39; g = 0xFF; b = 0x88; break; // Green
    }
    ACCENT      = RGB565(r, g, b);
    TEXT_DIM    = RGB565((uint8_t)(r * 45 / 100), (uint8_t)(g * 45 / 100), (uint8_t)(b * 45 / 100));
    ACCENT_DIM  = TEXT_DIM;
    ACCENT_GLOW = ACCENT;
    OK          = ACCENT;
}

const char* accentName(uint8_t preset) {
    switch (preset % ACCENT_PRESETS) {
        case 1:  return "Amber";
        case 2:  return "Cyan";
        case 3:  return "White";
        default: return "Green";
    }
}

} // namespace Theme
