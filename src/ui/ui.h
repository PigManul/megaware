#pragma once

#include "lgfx_config.h"
#include "features.h"
#include "../hal/board.h"
#include "../hal/battery.h"
#include "../net/wifi_manager.h"
#include "../storage/files.h"
#include "../scripting/lua_engine.h"
#include "../scripting/js_engine.h"

// =====================================================================
//  MEGAWARE UI — «Terminal-Lite»
//
//  Layout: header (logo + status) / content / footer (hints).
//  Navigation — vertical lists with a ">" cursor:
//    FocusLevel::Sidebar — main menu (sections with icons),
//    FocusLevel::SubTab  — list of a section's items,
//    FocusLevel::Field   — settings rows/lists/browser,
//    FocusLevel::Feature — feature screen (the plugin gets a content
//    rectangle and draws itself in the same palette),
//    FocusLevel::Modal   — on-screen keyboard / modal settings.
//
//  Input logic and the feature registry do not depend on the visuals.
// =====================================================================

// Main menu sections. A separate "Dashboard" no longer exists — Sleep/Reboot
// live in Settings (see builtinTabName). Indexes = menu order.
enum class Section { Wifi, SubGhz, Files, Bluetooth, Settings, COUNT };

enum class FocusLevel { Sidebar, SubTab, Field, Modal, Feature };
enum class ModalKind { None, WifiPassword };

class Ui {
public:
    void begin();
    void tick(const InputState& in);
    void showShutdown();
    LGFX& rawDisplay() { return display; } // for scripts (see src/scripting/game_io.*)
    bool featureActive() const { return activeFeature != nullptr; } // WebUI: do not touch the hardware while a feature owns it
    void webSetBrightness(uint8_t v);   // WebUI: apply brightness and save to NVS
    bool webSetAccentPreset(int idx);   // WebUI: apply the accent preset

private:
    LGFX display;
    LGFX_Sprite canvas{&display};

    bool splashDone = false;
    bool splashStarted = false;
    uint32_t bootAt = 0;
    bool timeConfigured = false;

    // --- navigation ---
    // Categories without items (e.g. Bluetooth with its features disabled)
    // are not shown in the list. sidebarIndex is the position in the VISIBLE
    // list; catOf() maps it to the registry category index, which is
    // used by subTabIndex/fieldIndex (per-category item memory).
    int sidebarIndex = 0;
    FocusLevel focusLevel = FocusLevel::Sidebar;
    int visibleCat[MW_BUILTIN_CATEGORY_COUNT + MW_MAX_NEW_CATEGORIES] = {0};
    int visibleCatCount = 0;
    int subTabIndex[(int)Section::COUNT + MW_MAX_NEW_CATEGORIES] = {0};
    int fieldIndex[(int)Section::COUNT + MW_MAX_NEW_CATEGORIES]  = {0};

    uint32_t subTabEnteredAt = 0;
    uint32_t lastKeyAt = 0;
    bool kbShowPlain = false;
    int  customizeOpen = -1;       // Settings->Customize: -1 list, 0 LED, 1 Display, 2 Theme
    int  netState = -1;
    uint32_t netAt = 0;
    bool netPending = false;

    // --- active feature (FocusLevel::Feature) ---
    const Feature* activeFeature = nullptr;
    int catOf(int pos) const;
    void rebuildVisibleCats();

    // --- LED settings (lots of customization: Brightness/R/G/B/Mode/Speed) ---
    uint8_t ledBrightness = 90;
    uint8_t ledR = 214, ledG = 24, ledB = 34;
    int ledModeIdx = 2; // 0=Off 1=Solid 2=Breathing 3=Tail 4=Rainbow — see LedMode
    uint8_t ledSpeedPct = 100;
    bool ledEditingRow = false; // nested navigation: false=row selection, true=editing the value

    // --- Display settings (Brightness/Auto-dim/Rotation) ---
    // Defaults are not "maxed out": IPS backlight at 100% is the main
    // source of screen heating. Persisted in NVS (disp_bright / disp_dim).
    uint8_t displayBrightness = 180;
    int displayAutoDimSec = 60;   // 0 = disabled
    bool displayFlipped = false;
    bool displayEditingRow = false;
    uint32_t lastActivityMs = 0;
    bool displayDimmedNow = false;

    int utcOffsetHours = 0;

    // --- Wi-Fi ---
    bool wifiScanTriggered = false;

    // --- Files ---
    String filesPath[2] = {"/", "/"};
    String lastScriptResult;
    uint32_t lastScriptResultAt = 0; // result line fades out after 4 s

    // --- audio playback from Files (hold on .mp3/.wav) ---
    // Implemented in ui.cpp (minimp3 + AudioOut I2S). Quit in Files
    // stops playback, a second Quit is navigation.
    bool playing = false;

    // --- customization (Settings -> Theme), persisted in NVS ---
    uint8_t themeAccentIdx = 0; // accent preset: 0 Green 1 Amber 2 Cyan 3 White
    bool footerHints = true;    // show hints in the footer

    // --- Functions (WiFi -> WebUI) ---
    bool webUiEnabled = false;
    int apPresetIdx = 0;
    bool apPresetLoaded = false;

    // --- Modal: on-screen QWERTY keyboard for the Wi-Fi password / WebUI settings ---
    ModalKind modalKind = ModalKind::None;
    String modalTargetSSID;
    String pwBuffer;
    int kbIndex = 0;
    int kbMode = 0;
    bool kbCaps = false;

    // --- redraw on demand (see tick()) ---
    // A full frame is ~106 KB over SPI; draw only when something
    // actually changed: input, a Wi-Fi/scan state change, or the
    // periodic tick (live screens 1 Hz, static ones 1/min).
    bool frameMoving = true;
    bool lastFrameAnimated = true;
    WifiConnState lastConnState = WifiConnState::Idle;
    bool lastScanning = false;
    uint32_t lastPeriodicDrawMs = 0;

    void handleInput(const InputState& in);
    void handleModalInput(const InputState& in);
    void noteActivity(const InputState& in); // reset screen auto-dim on any activity

    int  sectionTotal() const;
    int  builtinTabCount(Section s) const;         // built-in section items
    const char* builtinTabName(Section s, int i) const;
    bool sectionFieldEditable(Section s) const;
    bool sectionHasTabBar(int sectionIdx) const;
    int  itemCount(int sectionIdx) const;          // built-in items + features
    const char* itemName(int sectionIdx, int i) const;
    bool itemIsFeature(int sectionIdx, int i) const;
    void enterFeature(const Feature* f);
    void handleFeatureInput(const InputState& in);
    void startPlayback(const String& path);
    void stopPlayback();
    void tickPlayback(); // feeds I2S/DMA on every tick
    bool flashFirmwareFile(FsRoot root, const String& path); // hold on a .bin in Files
    void saveLedSettings(); // NVS: the whole Customize > LED screen

    // --- shared Terminal-Lite elements ---
    void drawScreen();            // header + content per focus level + footer
    void drawSplash();
    void drawHeader();                              // logo + wifi/battery/clock + line
    void drawFooter(const char* turn, const char* press, const char* hold); // line + hints
    void drawFooterDefault();                       // TURN: select / PRESS: ok / HOLD: back
    void contentRect(int& x, int& y, int& w, int& h);
    uint8_t iconForSection(int cat) const;          // section icon (MwIcon::Id)
    const char* sectionDisplayName(int cat) const;  // "Sub-GHz", "Wi-Fi", ...
    void drawScrollbar(int x, int y, int h, int top, int visible, int total);
    void drawCursor(int x, int y, bool selected);   // ">"
    void drawKV(int x, int y, int w, const char* key, const char* value, uint16_t valueCol); // "key: value"

    // --- screens ---
    void drawMainMenu();          // list of sections
    void drawSectionList();       // list of a section's items
    void drawFieldScreen();       // field-screen dispatcher by section/tab
    void drawLedScreen(int x, int y, int w, int h);
    void drawDisplayScreen(int x, int y, int w, int h);
    void drawTimeScreen(int x, int y, int w, int h);
    void drawThemeScreen(int x, int y, int w, int h); // accent/footer — customization
    void drawAboutScreen(int x, int y, int w, int h); // scrollable key:value
    int  aboutRowCount() const;
    void drawWifiScreen(int x, int y, int w, int h, int tab); // Connect / WebUI / Network
    void drawFilesScreen(int x, int y, int w, int h);
    void drawModalWifiKeyboard();
    void drawModalWebUiSettings();
    void drawPowerCountdown(uint32_t heldMs);
    void drawBatteryIcon(int x, int y);
    void drawWifiIcon(int x, int y, bool connected);

    void performDashboardAction(int index);
    void applyDisplayRotation();
};

extern Ui ui;
