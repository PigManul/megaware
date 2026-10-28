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
//  Компоновка: шапка (логотип + статус) / контент / подвал (подсказки).
//  Навигация — вертикальные списки с курсором «>»:
//    FocusLevel::Sidebar — главное меню (разделы с иконками),
//    FocusLevel::SubTab  — список пунктов раздела,
//    FocusLevel::Field   — строки настроек/списки/браузер,
//    FocusLevel::Feature — экран фичи (плагин получает контент-прямо-
//    угольник и рисует сама в той же палитре),
//    FocusLevel::Modal   — экранная клавиатура / модальные настройки.
//
//  Логика ввода и реестр фич не зависят от внешнего вида.
// =====================================================================

// Разделы главного меню. Отдельного «Dashboard» больше нет — Sleep/Reboot
// живут в Settings (см. builtinTabName). Индексы = порядок в меню.
enum class Section { Wifi, SubGhz, Files, Bluetooth, Settings, COUNT };

enum class FocusLevel { Sidebar, SubTab, Field, Modal, Feature };
enum class ModalKind { None, WifiPassword };

class Ui {
public:
    void begin();
    void tick(const InputState& in);
    void showShutdown();
    LGFX& rawDisplay() { return display; } // для скриптов (см. src/scripting/game_io.*)

private:
    LGFX display;
    LGFX_Sprite canvas{&display};

    bool splashDone = false;
    bool splashStarted = false;
    uint32_t bootAt = 0;
    bool timeConfigured = false;

    // --- навигация ---
    // Категории без пунктов (например, Bluetooth с выключенными фичами)
    // в списке не показываются. sidebarIndex — позиция в ВИДИМОМ
    // списке; catOf() переводит её в индекс категории реестра, который
    // используется в subTabIndex/fieldIndex (память пунктов на категорию).
    int sidebarIndex = 0;
    FocusLevel focusLevel = FocusLevel::Sidebar;
    int visibleCat[MW_BUILTIN_CATEGORY_COUNT + MW_MAX_NEW_CATEGORIES] = {0};
    int visibleCatCount = 0;
    int subTabIndex[(int)Section::COUNT + MW_MAX_NEW_CATEGORIES] = {0};
    int fieldIndex[(int)Section::COUNT + MW_MAX_NEW_CATEGORIES]  = {0};

    uint32_t subTabEnteredAt = 0;
    uint32_t lastKeyAt = 0;
    bool kbShowPlain = false;
    int  customizeOpen = -1;       // Settings->Customize: -1 список, 0 LED, 1 Display, 2 Theme
    int  netState = -1;
    uint32_t netAt = 0;
    bool netPending = false;

    // --- активная фича (FocusLevel::Feature) ---
    const Feature* activeFeature = nullptr;
    int catOf(int pos) const;
    void rebuildVisibleCats();

    // --- настройки LED (много кастомизации: Brightness/R/G/B/Mode/Speed) ---
    uint8_t ledBrightness = 90;
    uint8_t ledR = 214, ledG = 24, ledB = 34;
    int ledModeIdx = 2; // 0=Off 1=Solid 2=Breathing 3=Tail 4=Rainbow — см. LedMode
    uint8_t ledSpeedPct = 100;
    bool ledEditingRow = false; // вложенная навигация: false=выбор строки, true=редактирование значения

    // --- настройки Display (Brightness/Auto-dim/Rotation) ---
    // Дефолты не «на максимум»: подсветка IPS на 100% — главный источник
    // нагрева экрана. Персистятся в NVS (disp_bright / disp_dim).
    uint8_t displayBrightness = 180;
    int displayAutoDimSec = 60;   // 0 = выключено
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
    uint32_t lastScriptResultAt = 0; // строка результата гаснет через 4 с

    // --- проигрывание аудио из Files (hold на .mp3/.wav) ---
    // Реализация в ui.cpp (minimp3 + AudioOut I2S). Quit в Files
    // останавливает воспроизведение, повторный Quit — навигация.
    bool playing = false;

    // --- кастомизация (Settings -> Theme), персистится в NVS ---
    uint8_t themeAccentIdx = 0; // пресет акцента: 0 Green 1 Amber 2 Cyan 3 White
    bool footerHints = true;    // показывать подсказки в подвале

    // --- Functions (WiFi -> WebUI) ---
    bool webUiEnabled = false;
    int apPresetIdx = 0;
    bool apPresetLoaded = false;

    // --- Modal: экранная QWERTY-клавиатура для пароля Wi-Fi / настройки WebUI ---
    ModalKind modalKind = ModalKind::None;
    String modalTargetSSID;
    String pwBuffer;
    int kbIndex = 0;
    int kbMode = 0;
    bool kbCaps = false;

    // --- перерисовка по необходимости (см. tick()) ---
    // Полный кадр — это ~106 КБ по SPI; рисуем только когда что-то
    // реально изменилось: ввод, сменилось состояние Wi-Fi/скана или
    // периодика (живые экраны 1 Гц, статичные 1/мин).
    bool frameMoving = true;
    bool lastFrameAnimated = true;
    WifiConnState lastConnState = WifiConnState::Idle;
    bool lastScanning = false;
    uint32_t lastPeriodicDrawMs = 0;

    void handleInput(const InputState& in);
    void handleModalInput(const InputState& in);
    void noteActivity(const InputState& in); // сброс автозатемнения экрана при любой активности

    int  sectionTotal() const;
    int  builtinTabCount(Section s) const;         // встроенные пункты раздела
    const char* builtinTabName(Section s, int i) const;
    bool sectionFieldEditable(Section s) const;
    bool sectionHasTabBar(int sectionIdx) const;
    int  itemCount(int sectionIdx) const;          // встроенные пункты + фичи
    const char* itemName(int sectionIdx, int i) const;
    bool itemIsFeature(int sectionIdx, int i) const;
    void enterFeature(const Feature* f);
    void handleFeatureInput(const InputState& in);
    void startPlayback(const String& path);
    void stopPlayback();
    void tickPlayback(); // кормит I2S/DMA на каждом тике

    // --- общие элементы Terminal-Lite ---
    void drawScreen();            // шапка + контент по уровню фокуса + подвал
    void drawSplash();
    void drawHeader();                              // логотип + wifi/батарея/часы + линия
    void drawFooter(const char* turn, const char* press, const char* hold); // линия + подсказки
    void drawFooterDefault();                       // TURN: select / PRESS: ok / HOLD: back
    void contentRect(int& x, int& y, int& w, int& h);
    uint8_t iconForSection(int cat) const;          // иконка раздела (MwIcon::Id)
    const char* sectionDisplayName(int cat) const;  // «Sub-GHz», «Wi-Fi», ...
    void drawScrollbar(int x, int y, int h, int top, int visible, int total);
    void drawCursor(int x, int y, bool selected);   // «>»
    void drawKV(int x, int y, int w, const char* key, const char* value, uint16_t valueCol); // «ключ: значение»

    // --- экраны ---
    void drawMainMenu();          // список разделов
    void drawSectionList();       // список пунктов раздела
    void drawFieldScreen();       // диспетчер строковых экранов по разделу/вкладке
    void drawLedScreen(int x, int y, int w, int h);
    void drawDisplayScreen(int x, int y, int w, int h);
    void drawTimeScreen(int x, int y, int w, int h);
    void drawThemeScreen(int x, int y, int w, int h); // акцент/футер — кастомизация
    void drawAboutScreen(int x, int y, int w, int h); // прокручиваемый key:value
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
