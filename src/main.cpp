#include <Arduino.h>
#include "hal/board.h"
#include "hal/battery.h"
#include "hal/config.h"
#include "hal/memory_controller.h"
#include "ui/ui.h"
#include "radio/subghz.h"
#include "net/wifi_manager.h"
#include "storage/files.h"
#include "scripting/lua_engine.h"
#include "scripting/js_engine.h"
#include "../include/pins.h"

// =====================================================================
//  MEGAWARE — main.cpp
//
//  Порядок инициализации:
//    1) Board::powerOn()  — питание периферии + CS общей SPI-шины в HIGH.
//    2) ui.begin()        — дисплей (свой класс LGFX, SPI_MODE0).
//    3) Board::initInputs()/initLeds() — энкодер/кнопки/WS2812.
//    4) SubGhz::begin()   — CC1101.
//    5) Battery::begin()  — BQ27220 по I2C.
//    6) Config::begin()   — NVS готова к работе.
//    7) WifiMgr::begin()/tryAutoConnect() — Wi-Fi ленивый: радио НЕ
//       трогается, если нет сохранённой сети (экономия памяти).
//    8) Files::beginSD()/beginFlash() — SD-карта и внутренняя LittleFS.
// =====================================================================

uint32_t lastLedUpdate = 0;

void setup() {
    Serial.begin(115200);
    MemoryController::begin();

    Board::powerOn();
    ui.begin();

    Board::initInputs();
    Board::initLeds();

    bool radioOk = SubGhz::begin();
    if (!radioOk) {
        Serial.println("[MEGAWARE] WARNING: CC1101 not detected on SPI bus");
        Board::setLedColor(255, 40, 0);
        Board::setLedMode(LedMode::Solid);
    }

    Battery::begin();
    Config::begin();
    WifiMgr::begin();
    // Если есть сохранённая сеть — подключаемся сразу при загрузке
    // (креды пишутся при успешном подключении). Сеть не сохранена —
    // радио вообще не трогается, heap остаётся свободным.
    WifiMgr::tryAutoConnect();
    Files::beginSD();
    Files::beginFlash();
    LuaEngine::begin();
    JsEngine::begin();

    Serial.println("[MEGAWARE] boot complete");
}

void loop() {
    InputState in = Board::pollInput();

    ui.tick(in);
    WifiMgr::serviceAutoConnect(); // ретраи автоконнекта (сеть не сразу видна)
    if (in.powerHeldMs >= 3000) {
        delay(80); // дать последнему кадру "3/3" реально уйти на экран
        Board::enterDeepSleep();
        return; // недостижимо
    }

    uint32_t now = millis();
    MemoryController::sample();
    if (now - lastLedUpdate > 16) { // ~60 Гц для плавного "дыхания"
        Board::updateLeds(now);
        lastLedUpdate = now;
    }

    // Отдаём слайс IDLE-задаче: без этого loop крутится вхолостую на
    // полном газу (тысячи итераций в секунду) и планировщик никогда не
    // спит. Ввод не теряется: вращение копится в ISR энкодера, задержка
    // ≤1 мс на клик незаметна, WebUI опрашивается так же часто.
    delay(1);
}
