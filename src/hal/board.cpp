#include "board.h"
#include "../../include/pins.h"
#include <FastLED.h>
#include <esp_sleep.h>

#define MW_LEDS_ENABLED 1

namespace {
    // --- table-based half-step quadrature encoder decoder (Ben Buxton) ---
    constexpr uint8_t R_START        = 0x0;
    constexpr uint8_t R_CCW_BEGIN    = 0x1;
    constexpr uint8_t R_CW_BEGIN     = 0x2;
    constexpr uint8_t R_START_M      = 0x3;
    constexpr uint8_t R_CW_BEGIN_M   = 0x4;
    constexpr uint8_t R_CCW_BEGIN_M  = 0x5;
    constexpr uint8_t DIR_CW         = 0x10;
    constexpr uint8_t DIR_CCW        = 0x20;

    const uint8_t ttable[6][4] = {
        {R_START_M,                     R_CW_BEGIN,   R_CCW_BEGIN,  R_START},
        {(uint8_t)(R_START_M | DIR_CCW), R_START,      R_CCW_BEGIN,  R_START},
        {(uint8_t)(R_START_M | DIR_CW),  R_CW_BEGIN,   R_START,      R_START},
        {R_START_M,                      R_CCW_BEGIN_M, R_CW_BEGIN_M, R_START},
        {R_START_M,                      R_START_M,    R_CW_BEGIN_M, (uint8_t)(R_START | DIR_CW)},
        {R_START_M,                      R_CCW_BEGIN_M, R_START_M,   (uint8_t)(R_START | DIR_CCW)},
    };

    constexpr int ENCODER_DIRECTION = -1; // direction flip for this particular encoder unit

    volatile uint8_t encState = R_START;
    volatile int32_t encAccum = 0;

    void IRAM_ATTR onEncoderIsr() {
        uint8_t a = digitalRead(PIN_ENCODER_A);
        uint8_t b = digitalRead(PIN_ENCODER_B);
        uint8_t pinState = (uint8_t)((b << 1) | a);
        encState = ttable[encState & 0x0F][pinState];
        uint8_t dir = encState & 0x30;
        if (dir == DIR_CW) encAccum += ENCODER_DIRECTION;
        else if (dir == DIR_CCW) encAccum -= ENCODER_DIRECTION;
    }

    bool keyPrevState = HIGH;
    uint32_t keyDownAt = 0;
    bool longFired = false;

    bool userKeyPrevState = HIGH;
    uint32_t userKeyDownAt = 0;

    CRGB leds[WS2812_NUM_LEDS];
    uint8_t ledR = 214, ledG = 24, ledB = 34;
    uint8_t ledBrightness = 90;
    uint8_t ledSpeedPct = 100;
    LedMode ledMode = LedMode::Breathing;
    // Off/Solid do not change the frame data — a single show() is enough,
    // not one every 16ms (each show blocks for the write duration).
    bool ledOffFlushed = false;
    bool ledSolidFlushed = false;

    // encoder feedback: blip on click, glow on rotation —
    // on top of any mode (including Off). markLedDirty() at start is needed
    // so Off/Solid repaint after the blip instead of staying stuck white.
    bool ledClickBlip = true;
    bool ledRotGlow = true;
    uint32_t blipUntil = 0;
    uint32_t rotUntil = 0;

    // Animated modes (Breathing/Tail): on slow parts of the wave
    // consecutive frames differ by less than 3 of 255 levels —
    // such a show() changes nothing visually but blocks interrupts
    // during the write (8 LEDs ≈ 0.6 ms every 16 ms). We compare against
    // the previous frame and write only real changes to the LEDs.
    uint8_t prevLeds[WS2812_NUM_LEDS][3];
    bool ledFrameFlushed = false;
    void markLedDirty() { ledOffFlushed = false; ledSolidFlushed = false; ledFrameFlushed = false; }
    void flushLedsIfChanged() {
        bool changed = !ledFrameFlushed;
        for (int i = 0; i < WS2812_NUM_LEDS && !changed; i++)
            for (int ch = 0; ch < 3; ch++)
                if (abs((int)leds[i][ch] - (int)prevLeds[i][ch]) > 2) { changed = true; break; }
        if (changed) {
            FastLED.show();
            memcpy(prevLeds, leds, sizeof(prevLeds));
            ledFrameFlushed = true;
        }
    }

    constexpr uint32_t CLICK_MAX_MS   = 350;
    constexpr uint32_t LONGPRESS_MS   = 600;
}

void Board::powerOn() {
    pinMode(PIN_BOARD_PWR_EN, OUTPUT);
    digitalWrite(PIN_BOARD_PWR_EN, HIGH);

    // Raise the shared SPI bus CS lines (CC1101/SD) HIGH BEFORE display
    // init — otherwise a floating CS may disturb the TFT on the shared bus.
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    delay(50);
}

void Board::initInputs() {
    pinMode(PIN_ENCODER_KEY, INPUT_PULLUP);
    pinMode(PIN_USER_KEY, INPUT_PULLUP);
    pinMode(PIN_ENCODER_A, INPUT_PULLUP);
    pinMode(PIN_ENCODER_B, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), onEncoderIsr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B), onEncoderIsr, CHANGE);
}

void Board::initLeds() {
#if MW_LEDS_ENABLED
    FastLED.addLeds<WS2812, PIN_WS2812_DATA, GRB>(leds, WS2812_NUM_LEDS);
    FastLED.setBrightness(ledBrightness);
    FastLED.clear(true);
#endif
}

InputState Board::pollInput() {
    InputState st;

    noInterrupts();
    int32_t delta = encAccum;
    encAccum = 0;
    interrupts();
    if (delta != 0) st.rotationSteps = (int)delta;

    uint32_t now = millis();

    bool keyState = digitalRead(PIN_ENCODER_KEY);
    if (keyPrevState == HIGH && keyState == LOW) {
        keyDownAt = now;
        longFired = false;
    } else if (keyState == LOW && !longFired && (now - keyDownAt) > LONGPRESS_MS) {
        longFired = true;
        keyPrevState = keyState;
        st.event = InputEvent::LongPress;
        return st;
    } else if (keyPrevState == LOW && keyState == HIGH) {
        uint32_t held = now - keyDownAt;
        keyPrevState = keyState;
        if (!longFired && held < CLICK_MAX_MS) st.event = InputEvent::Click;
        return st;
    }
    keyPrevState = keyState;

    bool ukState = digitalRead(PIN_USER_KEY);
    if (userKeyPrevState == HIGH && ukState == LOW) {
        userKeyDownAt = now;
    }
    if (ukState == LOW) {
        st.powerHeldMs = now - userKeyDownAt;
    } else if (userKeyPrevState == LOW && ukState == HIGH) {
        uint32_t held = now - userKeyDownAt;
        userKeyPrevState = ukState;
        if (held < CLICK_MAX_MS) st.event = InputEvent::UserKey;
        return st;
    }
    userKeyPrevState = ukState;

    return st;
}

void Board::setLedColor(uint8_t r, uint8_t g, uint8_t b) { if (ledR != r || ledG != g || ledB != b) markLedDirty(); ledR = r; ledG = g; ledB = b; }
void Board::setLedMode(LedMode m)  { if (ledMode != m) markLedDirty(); ledMode = m; }
void Board::setLedSpeed(uint8_t speedPct) { ledSpeedPct = constrain(speedPct, (uint8_t)10, (uint8_t)300); }
LedMode Board::getLedMode()  { return ledMode; }
uint8_t Board::getLedR() { return ledR; }
uint8_t Board::getLedG() { return ledG; }
uint8_t Board::getLedB() { return ledB; }
uint8_t Board::getLedSpeed() { return ledSpeedPct; }

void Board::setLedBrightness(uint8_t v) {
    if (ledBrightness != v) markLedDirty();
    ledBrightness = v;
#if MW_LEDS_ENABLED
    FastLED.setBrightness(v);
#endif
}

void Board::setLedClickBlip(bool on) { ledClickBlip = on; if (!on) blipUntil = 0; }
void Board::setLedRotGlow(bool on)  { ledRotGlow = on; if (!on) rotUntil = 0; }
bool Board::getLedClickBlip() { return ledClickBlip; }
bool Board::getLedRotGlow()   { return ledRotGlow; }

void Board::ledBlip() {
    if (!ledClickBlip) return;
    blipUntil = millis() + 220;
    markLedDirty();
}

void Board::ledRot() {
    if (!ledRotGlow) return;
    rotUntil = millis() + 90;
    markLedDirty();
}

void Board::updateLeds(uint32_t nowMs) {
#if MW_LEDS_ENABLED
    // blip (click) / glow (rotation) override any mode
    if (nowMs < blipUntil || nowMs < rotUntil) {
        if (nowMs < blipUntil) {
            for (int i = 0; i < WS2812_NUM_LEDS; i++) leds[i] = CRGB(255, 255, 255);
        } else {
            CRGB glow((uint8_t)(ledR / 2 + 96), (uint8_t)(ledG / 2 + 96), (uint8_t)(ledB / 2 + 96));
            for (int i = 0; i < WS2812_NUM_LEDS; i++) leds[i] = glow;
        }
        flushLedsIfChanged();
        return;
    }
    CRGB base(ledR, ledG, ledB);
    uint32_t t = (uint32_t)((uint64_t)nowMs * ledSpeedPct / 100); // Speed scales the "internal" animation time

    switch (ledMode) {
        case LedMode::Off:
            if (!ledOffFlushed) { FastLED.clear(); FastLED.show(); ledOffFlushed = true; }
            return;

        case LedMode::Solid:
            if (!ledSolidFlushed) {
                for (int i = 0; i < WS2812_NUM_LEDS; i++) leds[i] = base;
                FastLED.show();
                ledSolidFlushed = true;
            }
            return;

        case LedMode::Tail: {
            constexpr uint32_t STEP_MS = 120;
            int head = (int)((t / STEP_MS) % WS2812_NUM_LEDS);
            for (int i = 0; i < WS2812_NUM_LEDS; i++) {
                int dist = head - i;
                if (dist < 0) dist += WS2812_NUM_LEDS;
                float fade = 1.0f - (float)dist / 3.5f;
                fade = constrain(fade, 0.0f, 1.0f);
                leds[i] = CRGB((uint8_t)(base.r * fade), (uint8_t)(base.g * fade), (uint8_t)(base.b * fade));
            }
            flushLedsIfChanged();
            return;
        }

        case LedMode::Rainbow: {
            uint8_t hueBase = (uint8_t)((t / 20) % 256);
            for (int i = 0; i < WS2812_NUM_LEDS; i++) {
                CHSV hsv((uint8_t)(hueBase + i * (256 / WS2812_NUM_LEDS)), 255, 255);
                hsv2rgb_rainbow(hsv, leds[i]);
            }
            flushLedsIfChanged(); // Rainbow changes every frame — will never skip a show()
            return;
        }

        case LedMode::Breathing:
        default: {
            float phase = (float)(t % 1800) / 1800.0f;
            float wave = (sinf(phase * 2.0f * PI) * 0.5f) + 0.5f;
            for (int i = 0; i < WS2812_NUM_LEDS; i++) {
                float local = wave * (0.55f + 0.45f * sinf((phase + (float)i / WS2812_NUM_LEDS) * 2.0f * PI));
                local = constrain(local, 0.08f, 1.0f);
                leds[i] = CRGB((uint8_t)(base.r * local), (uint8_t)(base.g * local), (uint8_t)(base.b * local));
            }
            flushLedsIfChanged();
            return;
        }
    }
#endif
}

float Board::readBatteryVoltageApprox() { return 0.0f; }

void Board::enterDeepSleep() {
#if MW_LEDS_ENABLED
    FastLED.clear(true);
#endif
    detachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A));
    detachInterrupt(digitalPinToInterrupt(PIN_ENCODER_B));

    // Wakeup is on the ENCODER button (GPIO0), NOT on Quit (GPIO6):
    // these are different pins, so there is no need to wait for Quit to
    // be released — if the wakeup pin matched a held-down button, the
    // EXT0-wakeup would fire instantly (condition already true at sleep entry).
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ENCODER_KEY, 0);
    esp_deep_sleep_start();
}
