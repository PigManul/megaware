#include "features.h"
#include "../../include/theme.h"

// =====================================================================
//  Feature registry: static arrays, sorted once in init().
//  All state is constant-initialized (POD) — the order of static
//  constructors across feature files does not matter (no static init fiasco).
// =====================================================================

namespace {
    Feature      regs[MW_MAX_FEATURES];
    int          regCount = 0;
    bool         inited = false;
    volatile bool redrawReq = false;

    // After init(): regs is sorted by (category, order, registration),
    // catStart[c]..catStart[c+1] is the range of category c features in regs.
    int catStart[MW_BUILTIN_CATEGORY_COUNT + MW_MAX_NEW_CATEGORIES + 1];

    struct NewCategory {
        const char* name;
        uint8_t     icon;
        int8_t      bestOrder; // the feature with the lower order provides the category icon
    };
    NewCategory newCats[MW_MAX_NEW_CATEGORIES];
    int         newCatCount = 0;

    // ORDER must match enum Section in ui.h:
    // { Wifi, SubGhz, Files, Bluetooth, Settings }
    const char* const builtinNames[MW_BUILTIN_CATEGORY_COUNT] = {
        MwCat::Wifi, MwCat::SubGhz, MwCat::Files,
        MwCat::Bluetooth, MwCat::Settings,
    };

    // Feature category slot: 0..5 — built-in, 6+ — new (created on first
    // appearance), -1 — the category accepts no features (Dashboard —
    // it has no tabs, its items would be unreachable).
    int categorySlotOf(const Feature& f) {
        if (!f.name || !f.name[0] || !f.category || !f.category[0]) return -1;
        for (int i = 0; i < MW_BUILTIN_CATEGORY_COUNT; i++)
            if (strcmp(f.category, builtinNames[i]) == 0) return i;
        for (int i = 0; i < newCatCount; i++)
            if (strcmp(f.category, newCats[i].name) == 0) {
                if (f.order < newCats[i].bestOrder) { // the earliest feature provides the icon
                    newCats[i].icon = f.icon;
                    newCats[i].bestOrder = f.order;
                }
                return MW_BUILTIN_CATEGORY_COUNT + i;
            }
        if (newCatCount >= MW_MAX_NEW_CATEGORIES) return -1;
        newCats[newCatCount].name = f.category;
        newCats[newCatCount].icon = f.icon;
        newCats[newCatCount].bestOrder = f.order;
        return MW_BUILTIN_CATEGORY_COUNT + newCatCount++;
    }
}

namespace Features {

void registerFeature(const Feature& f) {
    if (regCount < MW_MAX_FEATURES) regs[regCount++] = f;
}

void init() {
    if (inited) return;
    inited = true;

    // Slot layout + rejection of broken/invalid features.
    int slots[MW_MAX_FEATURES];
    int kept = 0;
    for (int i = 0; i < regCount; i++) {
        int slot = categorySlotOf(regs[i]);
        if (slot < 0 || !regs[i].onDraw) {
            Serial.printf("[MEGAWARE] feature '%s' skipped (bad category=%s / no onDraw)\n",
                          regs[i].name ? regs[i].name : "?", regs[i].category ? regs[i].category : "?");
            continue;
        }
        regs[kept] = regs[i];
        slots[kept] = slot;
        kept++;
    }
    regCount = kept;

    // Stable insertion sort by (slot, order, original order).
    for (int i = 1; i < regCount; i++) {
        Feature f = regs[i];
        int s = slots[i];
        int j = i - 1;
        while (j >= 0 && (slots[j] > s || (slots[j] == s && regs[j].order > f.order))) {
            regs[j + 1] = regs[j];
            slots[j + 1] = slots[j];
            j--;
        }
        regs[j + 1] = f;
        slots[j + 1] = s;
    }

    // Category boundaries in the sorted array (prefix sums —
    // empty categories correctly yield featureCount == 0).
    int maxSlot = MW_BUILTIN_CATEGORY_COUNT + newCatCount;
    int counts[MW_BUILTIN_CATEGORY_COUNT + MW_MAX_NEW_CATEGORIES];
    for (int c = 0; c < maxSlot; c++) counts[c] = 0;
    for (int i = 0; i < regCount; i++) counts[slots[i]]++;
    catStart[0] = 0;
    for (int c = 0; c < maxSlot; c++) catStart[c + 1] = catStart[c] + counts[c];
}

int categoryCount() { return MW_BUILTIN_CATEGORY_COUNT + newCatCount; }

// Explicit sidebar order (MwOrder): built-ins and new ones interleaved.
int categoryOrder(int category) {
    static const char* const ORDER[] = {
        MwCat::Wifi, MwCat::SubGhz, "RF24", "NFC", "IR", "Ethernet",
        "Tools", "Others", MwCat::Files, MwCat::Settings,
    };
    const char* n = categoryName(category);
    for (int i = 0; i < (int)(sizeof(ORDER) / sizeof(ORDER[0])); i++)
        if (strcmp(n, ORDER[i]) == 0) return i;
    return 50 + category;
}

const char* categoryName(int category) {
    if (category < MW_BUILTIN_CATEGORY_COUNT) return builtinNames[category];
    if (category < MW_BUILTIN_CATEGORY_COUNT + newCatCount) return newCats[category - MW_BUILTIN_CATEGORY_COUNT].name;
    return "";
}

uint8_t categoryIcon(int category) {
    if (category >= MW_BUILTIN_CATEGORY_COUNT && category < MW_BUILTIN_CATEGORY_COUNT + newCatCount)
        return newCats[category - MW_BUILTIN_CATEGORY_COUNT].icon;
    return MwIcon::None;
}

int featureCount(int category) {
    if (category < 0 || category >= categoryCount()) return 0;
    return catStart[category + 1] - catStart[category];
}

const Feature* feature(int category, int item) {
    if (item < 0 || item >= featureCount(category)) return nullptr;
    return &regs[catStart[category] + item];
}

void requestRedraw() { redrawReq = true; }
bool takeRedrawRequest() {
    bool v = redrawReq;
    redrawReq = false;
    return v;
}

} // namespace Features

// =====================================================================
//  MwDraw — feature drawing helpers. "Terminal-Lite" styling: black
//  background, accent title instead of a panel, a horizontal bar instead
//  of a circular gauge. Features call card()/gauge() as before — the look
//  changes centrally here and in include/theme.h.
// =====================================================================

namespace MwDraw {

void card(LGFX_Sprite& c, int x, int y, int w, int h, const char* title) {
    (void)w; (void)h; // no more panels — just an accent-colored title
    if (title && title[0]) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::ACCENT, Theme::BG);
        c.drawString(title, x + 8, y + 2);
    }
}

void gauge(LGFX_Sprite& c, int cx, int cy, int r, float pct,
           const char* bigText, const char* smallText) {
    pct = constrain(pct, 0.0f, 1.0f);
    // value shown large + horizontal bar (r = bar half-width)
    c.setFont(&fonts::Font0);
    c.setTextSize(Theme::SIZE_MENU);
    c.setTextDatum(textdatum_t::middle_center);
    c.setTextColor(Theme::ACCENT, Theme::BG);
    c.drawString(bigText, cx, cy - 6);
    c.setTextSize(Theme::SIZE_TEXT);

    c.fillRect(cx - r, cy + 8, r * 2, 3, Theme::TEXT_DIM);
    c.fillRect(cx - r, cy + 8, (int)(r * 2 * pct), 3, Theme::ACCENT);

    c.setTextDatum(textdatum_t::middle_center);
    c.setTextColor(Theme::TEXT_DIM, Theme::BG);
    c.drawString(smallText, cx, cy + r + 8);
}

// Icons of new sidebar categories — simple vector glyphs ~16px,
// in the same style as the built-in icons of Ui::drawSidebarIcon().
void drawIcon(LGFX_Sprite& c, uint8_t iconId, int cx, int cy, uint16_t col) {
    switch (iconId) {
        case MwIcon::Grid: {
            int s = 5, g = 3;
            c.fillRoundRect(cx - g - s, cy - g - s, s, s, 1, col);
            c.fillRoundRect(cx + g,     cy - g - s, s, s, 1, col);
            c.fillRoundRect(cx - g - s, cy + g,     s, s, 1, col);
            c.fillRoundRect(cx + g,     cy + g,     s, s, 1, col);
            break;
        }
        case MwIcon::Antenna: {
            c.drawArc(cx, cy + 4, 4, 4, 200, 340, col);
            c.drawArc(cx, cy + 4, 8, 8, 200, 340, col);
            c.drawArc(cx, cy + 4, 12, 12, 200, 340, col);
            c.fillCircle(cx, cy + 4, 2, col);
            break;
        }
        case MwIcon::Wifi: {
            c.fillCircle(cx, cy + 5, 2, col);
            c.drawArc(cx, cy + 5, 6, 6, 210, 330, col);
            c.drawArc(cx, cy + 5, 10, 10, 210, 330, col);
            break;
        }
        case MwIcon::Folder: {
            c.fillRoundRect(cx - 8, cy - 4, 7, 4, 1, col);
            c.drawRoundRect(cx - 8, cy - 2, 16, 11, 2, col);
            break;
        }
        case MwIcon::Bluetooth: {
            c.drawLine(cx, cy - 8, cx, cy + 8, col);
            c.drawLine(cx, cy - 8, cx + 5, cy - 4, col);
            c.drawLine(cx + 5, cy - 4, cx - 5, cy + 4, col);
            c.drawLine(cx - 5, cy + 4, cx, cy + 8, col);
            c.drawLine(cx, cy - 8, cx - 5, cy - 4, col);
            c.drawLine(cx - 5, cy - 4, cx + 5, cy + 4, col);
            c.drawLine(cx + 5, cy + 4, cx, cy + 8, col);
            break;
        }
        case MwIcon::Gear: {
            c.drawCircle(cx, cy, 7, col);
            c.fillCircle(cx, cy, 2, col);
            for (int a = 0; a < 360; a += 45) {
                float rad = a * PI / 180.0f;
                int x = cx + (int)(cosf(rad) * 10);
                int y = cy + (int)(sinf(rad) * 10);
                c.fillRect(x - 1, y - 1, 3, 3, col);
            }
            break;
        }
        case MwIcon::Chip: {
            c.drawRoundRect(cx - 7, cy - 7, 14, 14, 2, col);
            c.fillRoundRect(cx - 3, cy - 3, 6, 6, 1, col);
            for (int i = -4; i <= 4; i += 4) {
                c.fillRect(cx + i - 1, cy - 10, 2, 3, col);  // top
                c.fillRect(cx + i - 1, cy + 7, 2, 3, col);   // bottom
                c.fillRect(cx - 10, cy + i - 1, 3, 2, col);  // left
                c.fillRect(cx + 7, cy + i - 1, 3, 2, col);   // right
            }
            break;
        }
        case MwIcon::Terminal: {
            c.drawRoundRect(cx - 9, cy - 7, 18, 14, 2, col);
            c.drawLine(cx - 6, cy - 2, cx - 2, cy + 2, col);
            c.drawLine(cx - 2, cy + 2, cx - 6, cy + 5 - 3, col);
            c.fillRect(cx + 1, cy + 3, 5, 2, col);
            break;
        }
        case MwIcon::Battery: {
            c.drawRoundRect(cx - 9, cy - 4, 16, 9, 2, col);
            c.fillRect(cx + 8, cy - 1, 2, 3, col);
            c.fillRect(cx - 7, cy - 2, 8, 5, col);
            break;
        }
        case MwIcon::Pulse: {
            c.drawLine(cx - 9, cy, cx - 4, cy, col);
            c.drawLine(cx - 4, cy, cx - 2, cy - 5, col);
            c.drawLine(cx - 2, cy - 5, cx + 1, cy + 5, col);
            c.drawLine(cx + 1, cy + 5, cx + 3, cy, col);
            c.drawLine(cx + 3, cy, cx + 9, cy, col);
            break;
        }
        case MwIcon::Nrf: { // zigzag radio (to distinguish from the SubGhz/Wi-Fi antennas)
            c.drawLine(cx - 8, cy + 5, cx - 3, cy + 5, col);
            c.drawLine(cx - 3, cy + 5, cx, cy - 6, col);
            c.drawLine(cx, cy - 6, cx + 3, cy + 5, col);
            c.drawLine(cx + 3, cy + 5, cx + 8, cy + 5, col);
            c.fillRect(cx + 6, cy - 7, 2, 9, col);
            break;
        }
        case MwIcon::Ethernet: { // RJ45
            c.drawRoundRect(cx - 9, cy - 3, 18, 9, 1, col);
            for (int i = 0; i < 4; i++) c.fillRect(cx - 6 + i * 4, cy - 7, 2, 4, col);
            break;
        }
        case MwIcon::Screwdriver: { // screwdriver
            c.drawLine(cx - 2, cy - 8, cx + 4, cy - 2, col);
            c.drawLine(cx + 2, cy - 8, cx - 4, cy - 2, col);
            c.drawLine(cx - 4, cy - 2, cx + 2, cy + 6, col);
            c.fillRect(cx - 1, cy + 4, 6, 5, col);
            break;
        }
        case MwIcon::Others: { // three dots
            c.fillCircle(cx - 6, cy, 2, col);
            c.fillCircle(cx, cy, 2, col);
            c.fillCircle(cx + 6, cy, 2, col);
            break;
        }
        case MwIcon::Lock: {
            c.drawArc(cx, cy - 2, 4, 5, 180, 360, col);
            c.fillRoundRect(cx - 6, cy - 2, 12, 10, 2, col);
            c.fillCircle(cx, cy + 3, 1, Theme::BG_PANEL);
            break;
        }
        default:
            // MwIcon::None — placeholder dot so the sidebar slot is not empty
            c.fillCircle(cx, cy, 3, col);
            break;
    }
}

} // namespace MwDraw
