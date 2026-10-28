#pragma once
#include <Arduino.h>
#include "lgfx_config.h"
#include "../hal/board.h"

// =====================================================================
//  MEGAWARE feature registry — подключение функций одним файлом.
//
//  Функция (фича) = один .cpp в src/features/, который макросом
//  REGISTER_FEATURE объявляет: название, категорию GUI, иконку,
//  порядок и обработчики (вход/ввод/цикл/рисование/выход).
//
//  Категория = секция сайдбара. Совпадает со встроенной (MwCat::...) —
//  фича дописывается пунктом в её подвкладки; не совпадает — новая
//  секция сайдбара создаётся автоматически (иконка — иконка фичи).
//
//  Реестр целиком в статической памяти (никаких heap-аллокаций),
//  заполняется статическими конструкторами до setup() и сортируется
//  один раз в init() (вызывается из Ui::begin()).
//
//  Почему линкер не выбрасывает регистрацию: объектники из src/
//  PlatformIO линкует напрямую (не архивом), а конструкторы лежат в
//  .init_array, который линкер-скрипты ESP-IDF держат через KEEP.
//  Проверено сборкой — имя фичи присутствует в firmware.bin.
// =====================================================================

// Имена встроенных категорий. ПОРЯДОК обязан совпадать с enum Section
// в src/ui/ui.h — индекс сайдбара = индекс категории реестра.
namespace MwCat {
    constexpr const char* Dashboard = "Dashboard";
    constexpr const char* SubGhz    = "SubGhz";
    constexpr const char* Wifi      = "Wifi";
    constexpr const char* Files     = "Files";
    constexpr const char* Bluetooth = "Bluetooth";
    constexpr const char* Settings  = "Settings";
}

constexpr int MW_BUILTIN_CATEGORY_COUNT = 5; // Wifi, SubGhz, Files, Bluetooth, Settings
constexpr int MW_MAX_NEW_CATEGORIES     = 6;   // новых категорий сайдбара
constexpr int MW_MAX_FEATURES           = 32;  // всего фич (память: ~28 Б/шт, static)

// Явный порядок категорий в сайдбаре (index = позиция сверху).
// Восстанавливается по имени: builtin'ы и новые категории перемешаны.
namespace MwOrder {
    // 1 Wi-Fi, 2 SubGHz, 3 RF24, 4 NFC, 5 IR, 6 Ethernet,
    // 7 Others (между Ethernet и Files — отмечено в отчёте),
    // 8 Files, 9 Settings.
}

// Готовые векторные иконки для НОВЫХ категорий (рисуются в стиле
// встроенных, см. MwDraw::drawIcon). Для фичи внутри существующей
// категории иконка не используется — ставь MwIcon::None.
namespace MwIcon {
    enum Id : uint8_t {
        None = 0, Grid, Antenna, Wifi, Folder, Bluetooth, Gear,
        Chip, Terminal, Battery, Pulse, Lock,
        Nrf, Ethernet, Others
    };
}

struct Feature {
    const char* name;      // название пункта (коротко, шрифт Font0)
    const char* category;  // имя категории: MwCat::* или своё ("Tools" и т.п.)
    uint8_t     icon;      // MwIcon::Id — только для НОВОЙ категории
    int8_t      order;     // порядок в категории: меньше — раньше; равные — по порядку регистрации

    void  (*onEnter)();                                    // вход в функцию (опц.)
    bool  (*onInput)(const InputState& in);                // ввод; true = событие съедено (опц.)
    void  (*onTick)();                                     // цикл/обновление — каждый тик loop() (опц.)
    void  (*onDraw)(LGFX_Sprite& c, int x, int y, int w, int h); // контент карточки (опц.)
    void  (*onExit)();                                     // выход из функции (опц.)
};

// Валидация + раскладка по категориям. Руками не вызывать — юзай макрос.
namespace Features {
    void registerFeature(const Feature& f);
    void init(); // один раз из Ui::begin(): сортировка, карта категорий, WARN о битых фичах

    int         categoryCount();                  // встроенные + новые
    const char* categoryName(int category);
    uint8_t     categoryIcon(int category);       // имеет смысл для новых категорий
    int         categoryOrder(int category);      // позиция в сайдбаре (см. MwOrder)
    int         featureCount(int category);       // фич-пунктов в категории
    const Feature* feature(int category, int item); // item: 0..featureCount-1

    void requestRedraw();      // фича: перерисовать кадр при следующем тике
    bool takeRedrawRequest();  // Ui: забрать и сбросить флаг
}

// Тонкие помощники отрисовки для фич (та же стилистика, что у Ui;
// Ui::drawCard/drawGauge делегируют сюда — код один, вид одинаковый).
namespace MwDraw {
    void card(LGFX_Sprite& c, int x, int y, int w, int h, const char* title);
    void gauge(LGFX_Sprite& c, int cx, int cy, int r, float pct,
               const char* bigText, const char* smallText);
    void drawIcon(LGFX_Sprite& c, uint8_t iconId, int cx, int cy, uint16_t color);
}

struct FeatureRegistrar {
    explicit FeatureRegistrar(const Feature& f) { Features::registerFeature(f); }
};

// --- Регистрация функции: один макрос на один .cpp. Неиспользуемые
//     обработчики передавай как nullptr. Живой пример —
//     src/features/_template.cpp.example и docs/ADDING_FEATURES.md. ---
#define MW_REG2_(a, b) a##b
#define MW_REG_(a, b)  MW_REG2_(a, b)
#define REGISTER_FEATURE(name_, category_, icon_, order_, enter_, input_, tick_, draw_, exit_) \
    static const ::FeatureRegistrar MW_REG_(mw_feature_reg_, __COUNTER__) { \
        ::Feature{ name_, category_, (uint8_t)(icon_), (int8_t)(order_), \
                   enter_, input_, tick_, draw_, exit_ } \
    }
