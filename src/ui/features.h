#pragma once
#include <Arduino.h>
#include "lgfx_config.h"
#include "../hal/board.h"

// =====================================================================
//  MEGAWARE feature registry — plug in a function with a single file.
//
//  A feature = one .cpp in src/features/ which, via the REGISTER_FEATURE
//  macro, declares: name, GUI category, icon, order and handlers
//  (enter/input/tick/draw/exit).
//
//  Category = sidebar section. Matches a built-in one (MwCat::...) —
//  the feature is appended as an item to its subtabs; no match — a new
//  sidebar section is created automatically (icon = the feature's icon).
//
//  The registry lives entirely in static memory (no heap allocations),
//  is populated by static constructors before setup() and sorted once
//  in init() (called from Ui::begin()).
//
//  Why the linker does not drop the registration: object files from
//  src/ are linked by PlatformIO directly (not as an archive), and the
//  constructors live in .init_array, which ESP-IDF linker scripts keep
//  via KEEP. Verified by build — the feature name is present in firmware.bin.
// =====================================================================

// Built-in category names. ORDER must match enum Section
// in src/ui/ui.h — sidebar index = registry category index.
namespace MwCat {
    constexpr const char* Dashboard = "Dashboard";
    constexpr const char* SubGhz    = "SubGhz";
    constexpr const char* Wifi      = "Wifi";
    constexpr const char* Files     = "Files";
    constexpr const char* Bluetooth = "Bluetooth";
    constexpr const char* Settings  = "Settings";
}

constexpr int MW_BUILTIN_CATEGORY_COUNT = 5; // Wifi, SubGhz, Files, Bluetooth, Settings
constexpr int MW_MAX_NEW_CATEGORIES     = 6;   // new sidebar categories
constexpr int MW_MAX_FEATURES           = 32;  // total features (memory: ~28 B each, static)

// Explicit category order in the sidebar (index = position from top).
// Restored by name: built-ins and new categories are interleaved.
namespace MwOrder {
    // 1 Wi-Fi, 2 SubGHz, 3 RF24, 4 NFC, 5 IR, 6 Ethernet,
    // 7 Others (between Ethernet and Files — noted in the report),
    // 8 Files, 9 Settings.
}

// Ready-made vector icons for NEW categories (drawn in the style
// of the built-ins, see MwDraw::drawIcon). For a feature inside an
// existing category the icon is unused — set MwIcon::None.
namespace MwIcon {
    enum Id : uint8_t {
        None = 0, Grid, Antenna, Wifi, Folder, Bluetooth, Gear,
        Chip, Terminal, Battery, Pulse, Lock,
        Nrf, Ethernet, Others, Screwdriver
    };
}

struct Feature {
    const char* name;      // item name (short, Font0 font)
    const char* category;  // category name: MwCat::* or your own ("Tools" etc.)
    uint8_t     icon;      // MwIcon::Id — only for a NEW category
    int8_t      order;     // order within the category: lower = earlier; ties = registration order

    void  (*onEnter)();                                    // feature entry (opt.)
    bool  (*onInput)(const InputState& in);                // input; true = event consumed (opt.)
    void  (*onTick)();                                     // loop/update — every tick of loop() (opt.)
    void  (*onDraw)(LGFX_Sprite& c, int x, int y, int w, int h); // card content (opt.)
    void  (*onExit)();                                     // feature exit (opt.)
};

// Validation + layout by category. Do not call by hand — use the macro.
namespace Features {
    void registerFeature(const Feature& f);
    void init(); // once from Ui::begin(): sorting, category map, WARN about broken features

    int         categoryCount();                  // built-in + new
    const char* categoryName(int category);
    uint8_t     categoryIcon(int category);       // only meaningful for new categories
    int         categoryOrder(int category);      // position in the sidebar (see MwOrder)
    int         featureCount(int category);       // feature items in the category
    const Feature* feature(int category, int item); // item: 0..featureCount-1

    void requestRedraw();      // feature: redraw the frame on the next tick
    bool takeRedrawRequest();  // Ui: take and clear the flag
}

// Thin drawing helpers for features (same styling as Ui;
// Ui::drawCard/drawGauge delegate here — one code, identical look).
namespace MwDraw {
    void card(LGFX_Sprite& c, int x, int y, int w, int h, const char* title);
    void gauge(LGFX_Sprite& c, int cx, int cy, int r, float pct,
               const char* bigText, const char* smallText);
    void drawIcon(LGFX_Sprite& c, uint8_t iconId, int cx, int cy, uint16_t color);
}

struct FeatureRegistrar {
    explicit FeatureRegistrar(const Feature& f) { Features::registerFeature(f); }
};

// --- Feature registration: one macro per one .cpp. Pass unused
//     handlers as nullptr. Live example —
//     src/features/_template.cpp.example and docs/ADDING_FEATURES.md. ---
#define MW_REG2_(a, b) a##b
#define MW_REG_(a, b)  MW_REG2_(a, b)
#define REGISTER_FEATURE(name_, category_, icon_, order_, enter_, input_, tick_, draw_, exit_) \
    static const ::FeatureRegistrar MW_REG_(mw_feature_reg_, __COUNTER__) { \
        ::Feature{ name_, category_, (uint8_t)(icon_), (int8_t)(order_), \
                   enter_, input_, tick_, draw_, exit_ } \
    }
