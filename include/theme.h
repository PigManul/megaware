#pragma once
#include <cstdint>

constexpr uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// =====================================================================
//  MEGAWARE UI THEME — «Terminal-Lite»
//  Вся палитра интерфейса в одном месте. Меняй цвета только здесь.
//
//  Фон — чистый чёрный, панелей/рамок/скруглений нет. Основной акцент —
//  фосфорно-зелёный. Второстепенный текст — приглушённый зелёный.
//  Красный/янтарный — только ошибки и предупреждения.
//
//  Имена BG_PANEL / BG_PANEL_HI / BORDER сохранены как алиасы (историческое
//  имя «панели»), чтобы существующий код фич компилировался как есть —
//  визуально панелей больше нет: фон везде чёрный.
// =====================================================================

namespace Theme {
    // --- статичные цвета (от пресетов не зависят) ---
    constexpr uint16_t BG          = RGB565(0,   0,   0);    // фон везде
    constexpr uint16_t TEXT        = RGB565(0xDC,0xFF,0xEA); // значения: почти белый
    constexpr uint16_t WARN        = RGB565(0xFF,0xB0,0x20); // янтарный — редко, предупреждения
    constexpr uint16_t ERR         = RGB565(0xFF,0x50,0x50); // красный — только ошибки

    // --- акцентная группа: МЕНЯЕТСЯ пресетом из Settings -> Theme
    //     (Theme::setAccent, персистится в NVS). Не константы!
    extern uint16_t ACCENT;      // основной акцент (#39FF88 по умолчанию)
    extern uint16_t TEXT_DIM;    // приглушённый текст (~45% акцента)
    extern uint16_t OK;          // статусы «OK/mounted» = акцент
    extern uint16_t ACCENT_DIM;  // «тусклый акцент» = приглушённый
    extern uint16_t ACCENT_GLOW; // «свечение» = акцент

    void setAccent(uint8_t preset);     // 0 Green 1 Amber 2 Cyan 3 White
    const char* accentName(uint8_t preset);
    constexpr uint8_t ACCENT_PRESETS = 4;

    // --- алиасы совместимости (старые «панели» больше не рисуются) ---
    constexpr uint16_t BG_PANEL    = BG;
    constexpr uint16_t BG_PANEL_HI = BG;
    constexpr uint16_t BORDER      = BG;
    constexpr uint16_t TEXT_FAINT  = RGB565(0x1A,0x73,0x3D);

    constexpr uint16_t BLACK       = BG;
    constexpr uint16_t WHITE       = TEXT;

    // --- шрифт и метрики интерфейса (Terminal-Lite) ---
    // Один моноширинный пиксельный шрифт на всё: Font0 (5x7).
    // Размер 2 — пункты меню (крупный «терминальный» текст),
    // размер 1 — шапка, подвал, информационные строки.
    constexpr int SIZE_MENU   = 2;
    constexpr int SIZE_TEXT   = 1;

    constexpr int SCREEN_W    = 320;
    constexpr int SCREEN_H    = 170;
    constexpr int HEADER_H    = 20;   // шапка + линия под ней
    constexpr int FOOTER_H    = 16;   // линия + подсказки управления
    constexpr int PAD         = 8;    // общий боковой отступ
    constexpr int ROW_MENU    = 21;   // высота строки меню (шрифт x2)
    constexpr int ROW_LIST    = 14;   // высота строки списка/«ключ: значение»
    constexpr int LINE_W      = 1;    // разделительные линии
    constexpr int SCROLLBAR_W = 2;    // индикатор прокрутки справа
    constexpr int ICON_CELL   = 20;   // ширина ячейки под иконку в меню
    constexpr int KEY_COL_W   = 92;   // колонка «ключ» в информационных экранах

    constexpr int PADDING          = 8;   // алиас совместимости
    constexpr int HEADER_H_OLD     = 26;  // (не используется, оставлено для истории)
    constexpr int FOOTER_H_OLD     = 16;
    constexpr int ROW_H            = 34;
    constexpr uint32_t ANIM_MS     = 140;
    constexpr uint32_t PULSE_MS    = 1800;
}
