#include "subghz.h"
#include "../../include/pins.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    bool ready = false;
    float currentFreq = 433.92f;

    volatile RawCapture* capTarget = nullptr;
    volatile bool capturing = false;
    volatile uint32_t lastEdgeUs = 0;
    volatile uint32_t capStartMs = 0;
    volatile uint32_t capTimeoutMs = 0;

    void IRAM_ATTR gdo2Isr() {
        if (!capturing || capTarget == nullptr) return;
        uint32_t now = micros();
        uint32_t delta = now - lastEdgeUs;
        lastEdgeUs = now;
        if (delta < 40) return;
        if (capTarget->count >= RawCapture::MAX_EDGES) {
            capturing = false;
            return;
        }
        uint16_t clipped = (delta > 65000) ? 65000 : (uint16_t)delta;
        capTarget->durationsUs[capTarget->count++] = clipped;
    }
}

bool SubGhz::begin() {
    ELECHOUSE_cc1101.setSpiPin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_CC1101_CS);
    ELECHOUSE_cc1101.setGDO(PIN_CC1101_GDO0, PIN_CC1101_GDO2);

    pinMode(PIN_CC1101_SW0, OUTPUT);
    pinMode(PIN_CC1101_SW1, OUTPUT);

    ELECHOUSE_cc1101.Init();
    ready = ELECHOUSE_cc1101.getCC1101();

    if (ready) {
        setBand(SubGhzBand::MHz433);
        ELECHOUSE_cc1101.setCCMode(0);
        ELECHOUSE_cc1101.setModulation(2);
        ELECHOUSE_cc1101.setPA(10);
        ELECHOUSE_cc1101.SetRx();
    }
    return ready;
}

bool SubGhz::isReady() { return ready; }

void SubGhz::setBand(SubGhzBand band) {
    switch (band) {
        case SubGhzBand::MHz315:
            currentFreq = 315.00f;
            digitalWrite(PIN_CC1101_SW1, HIGH);
            digitalWrite(PIN_CC1101_SW0, LOW);
            break;
        case SubGhzBand::MHz433:
            currentFreq = 433.92f;
            digitalWrite(PIN_CC1101_SW1, HIGH);
            digitalWrite(PIN_CC1101_SW0, HIGH);
            break;
        case SubGhzBand::MHz868:
            currentFreq = 868.35f;
            digitalWrite(PIN_CC1101_SW1, LOW);
            digitalWrite(PIN_CC1101_SW0, HIGH);
            break;
    }
    if (ready) ELECHOUSE_cc1101.setMHZ(currentFreq);
}

float SubGhz::currentFrequencyMHz() { return currentFreq; }

void SubGhz::setFrequency(float mhz) {
    if (mhz < 300.0f || mhz > 930.0f) return;
    currentFreq = mhz;
    if (ready) ELECHOUSE_cc1101.setMHZ(currentFreq);
}

float SubGhz::readRssiDbm() {
    if (!ready) return -128.0f;
    return ELECHOUSE_cc1101.getRssi();
}

void SubGhz::startRawCapture(RawCapture* target, uint32_t timeoutMs) {
    if (!ready || target == nullptr) return;

    target->count = 0;
    target->valid = false;
    capTarget = target;
    capStartMs = millis();
    capTimeoutMs = timeoutMs;
    lastEdgeUs = micros();

    ELECHOUSE_cc1101.SetRx();
    pinMode(PIN_CC1101_GDO2, INPUT);
    capturing = true;
    attachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO2), gdo2Isr, CHANGE);
}

bool SubGhz::isCapturing() {
    if (!capturing) return false;
    if (millis() - capStartMs > capTimeoutMs) {
        stopRawCapture();
        return false;
    }
    return true;
}

void SubGhz::stopRawCapture() {
    // Detach even if the ISR already ended the capture (buffer full set
    // capturing=false): otherwise the GDO2 interrupt stays attached until
    // the next capture. No-op when nothing was ever attached.
    detachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO2));
    if (!capturing) return;
    capturing = false;
    if (capTarget) {
        capTarget->valid = capTarget->count > 4;
    }
}

bool SubGhz::replayRaw(const RawCapture& capture) {
    if (!ready || !capture.valid || capture.count == 0) return false;

    ELECHOUSE_cc1101.SetTx();
    pinMode(PIN_CC1101_GDO0, OUTPUT);

    bool level = HIGH;
    for (size_t i = 0; i < capture.count; i++) {
        digitalWrite(PIN_CC1101_GDO0, level ? HIGH : LOW);
        delayMicroseconds(capture.durationsUs[i]);
        level = !level;
    }
    digitalWrite(PIN_CC1101_GDO0, LOW);

    ELECHOUSE_cc1101.SetRx();
    return true;
}

bool SubGhz::saveRaw(const String& path, const RawCapture& capture) {
    if (!capture.valid || capture.count == 0) return false;
    if (SD.exists(path)) SD.remove(path);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) f = LittleFS.open(path, FILE_WRITE);
    if (!f) return false;
    f.println("Filetype: Flipper SubGhz Key File");
    f.println("Version: 1");
    f.println("Protocol: RAW");
    f.println("Frequency: " + String((unsigned long)(currentFreq * 1000000.0f)));
    f.println("RAW_Data:");
    for (size_t i = 0; i < capture.count; ++i) {
        if (i) f.print(' ');
        f.print(capture.durationsUs[i]);
    }
    f.println();
    f.close();
    return true;
}

bool SubGhz::loadRaw(const String& path, RawCapture& capture) {
    File f = SD.exists(path) ? SD.open(path) : LittleFS.open(path);
    if (!f) return false;
    bool raw = false;
    bool header = false;
    capture.count = 0;
    capture.valid = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line == "Filetype: Flipper SubGhz Key File") header = true;
        if (line.startsWith("Frequency:")) {
            long hz = line.substring(10).toInt();
            if (hz >= 100000000L && hz <= 1000000000L) setBand(hz < 400000000L ? SubGhzBand::MHz315 : (hz < 600000000L ? SubGhzBand::MHz433 : SubGhzBand::MHz868));
        }
        if (line == "RAW_Data:") { raw = true; continue; }
        if (!raw || !line.length()) continue;
        int start = 0;
        while (start < (int)line.length() && capture.count < RawCapture::MAX_EDGES) {
            while (start < (int)line.length() && line[start] == ' ') ++start;
            int end = line.indexOf(' ', start);
            if (end < 0) end = line.length();
            long value = line.substring(start, end).toInt();
            if (value > 0 && value <= 65000) capture.durationsUs[capture.count++] = (uint16_t)value;
            start = end + 1;
        }
    }
    f.close();
    capture.valid = header && capture.count > 4;
    return capture.valid;
}

RawCapture& SubGhz::sharedCapture() {
    static RawCapture capture;
    return capture;
}

// --- спектроанализатор ---
namespace {
    SubGhz::SpectrumData spectrum;
    int webSweepBand = -1; // -1 = веб не просит; иначе пресет диапазона

    const struct { float start, step; int count; } SWEEP_PRESETS[4] = {
        { 433.00f, 0.05f, 36 },  // 433 ISM
        { 868.00f, 0.025f, 25 }, // 868
        { 314.00f, 0.05f, 41 },  // 315
        { 902.00f, 0.20f, 66 },  // 915
    };
}

SubGhz::SpectrumData& SubGhz::sharedSpectrum() { return spectrum; }

void SubGhz::setSpectrumRange(float startMHz, float stepMHz, int count) {
    if (count > 128) count = 128;
    spectrum.startMHz = startMHz;
    spectrum.stepMHz = stepMHz;
    spectrum.count = count;
}

void SubGhz::webSpectrum(int band) { webSweepBand = band; }
bool SubGhz::webSpectrumActive() { return webSweepBand >= 0; }

void SubGhz::sweepOnce() {
    SpectrumData& s = spectrum;
    if (!ready || s.count <= 0) return;
    for (int i = 0; i < s.count; i++) {
        setFrequency(s.startMHz + i * s.stepMHz);
        delayMicroseconds(800); // на settling синтезатора CC1101
        s.dbm[i] = readRssiDbm();
    }
    s.lastUpdateMs = millis();
}

bool SubGhz::transmitFile(const String& path) {
    // формат Flipper (.sub): loadRaw сам ставит частоту из заголовка
    if (!loadRaw(path, sharedCapture())) return false;
    return replayRaw(sharedCapture());
}

bool SubGhz::transmitRaw(uint32_t hz, const String& raw) {
    if (hz < 300000000L || hz > 1000000000L) return false;
    setFrequency((float)hz / 1000000.0f);

    RawCapture& cap = sharedCapture();
    cap.count = 0;
    cap.valid = false;

    // "RAW_Data: 1000 -1000 500 -500 ..." или просто числа через пробел;
    // знаки игнорируются — replayRaw чередует высокий/низкий уровень
    String body = raw;
    int rIdx = body.indexOf("RAW_Data:");
    if (rIdx >= 0) body = body.substring(rIdx + 9);
    unsigned start = 0;
    while (start < body.length() && cap.count < RawCapture::MAX_EDGES) {
        while (start < body.length() && body[start] == ' ') ++start;
        if (start >= body.length()) break;
        int end = body.indexOf(' ', start);
        if (end < 0) end = body.length();
        long v = body.substring(start, end).toInt();
        start = end + 1;
        if (v == 0) continue;
        long mag = v < 0 ? -v : v;
        if (mag > 65000) mag = 65000;
        cap.durationsUs[cap.count++] = (uint16_t)mag;
    }
    cap.valid = cap.count > 0;
    if (!cap.valid) return false;
    return replayRaw(cap);
}
