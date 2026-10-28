#pragma once
#include <Arduino.h>

enum class InputEvent {
    None,
    Click,
    LongPress,
    UserKey
};

struct InputState {
    InputEvent event = InputEvent::None;
    int rotationSteps = 0;
    uint32_t powerHeldMs = 0; // 0, если Quit отпущена; иначе — сколько мс держится сейчас
};

enum class LedMode { Off, Solid, Breathing, Tail, Rainbow };

namespace Board {
    void powerOn();
    void initInputs();
    void initLeds();

    InputState pollInput();

    void updateLeds(uint32_t nowMs);
    void setLedColor(uint8_t r, uint8_t g, uint8_t b);
    void setLedMode(LedMode mode);
    void setLedBrightness(uint8_t v);
    void setLedSpeed(uint8_t speedPct); // 10..300, % от базовой скорости анимации
    LedMode getLedMode();
    uint8_t getLedR();
    uint8_t getLedG();
    uint8_t getLedB();
    uint8_t getLedSpeed();

    float readBatteryVoltageApprox();

    void enterDeepSleep();
}
