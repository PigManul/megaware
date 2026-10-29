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
    uint32_t powerHeldMs = 0; // 0 if Quit is released; otherwise how many ms it is held now
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
    void setLedSpeed(uint8_t speedPct); // 10..300, % of the base animation speed
    void setLedClickBlip(bool on);      // encoder blip on click (including over Off)
    void setLedRotGlow(bool on);        // encoder glow on rotation
    bool getLedClickBlip();
    bool getLedRotGlow();
    void ledBlip();                     // call from Ui::tick on click
    void ledRot();                      // ...on rotation
    LedMode getLedMode();
    uint8_t getLedR();
    uint8_t getLedG();
    uint8_t getLedB();
    uint8_t getLedSpeed();

    float readBatteryVoltageApprox();

    void enterDeepSleep();
}
