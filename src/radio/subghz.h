#pragma once
#include <Arduino.h>

enum class SubGhzBand : uint8_t { MHz315, MHz433, MHz868 };

struct RawCapture {
    static constexpr size_t MAX_EDGES = 2048;
    uint16_t durationsUs[MAX_EDGES];
    size_t   count = 0;
    bool     valid = false;
};

namespace SubGhz {
    bool begin();
    bool isReady();

    void setBand(SubGhzBand band);
    void setFrequency(float mhz);
    float currentFrequencyMHz();

    float readRssiDbm();

    void startRawCapture(RawCapture* target, uint32_t timeoutMs);
    bool isCapturing();
    void stopRawCapture();

    bool replayRaw(const RawCapture& capture);
    bool saveRaw(const String& path, const RawCapture& capture);
    bool loadRaw(const String& path, RawCapture& capture);

    // Bruce-совместимые передатчики (см. wiki.bruce.computer subghz):
    bool transmitFile(const String& path);             // .sub (Flipper) -> TX
    bool transmitRaw(uint32_t hz, const String& raw);  // hz (300M..1G) + "RAW_Data: 1000 -1000 ..."

    // --- спектроанализатор: общие данные фичи и WebUI (/api/spectrum) ---
    struct SpectrumData {
        float startMHz = 433.05f;
        float stepMHz = 0.05f;
        int   count = 0;                 // точек в развёртке
        float dbm[128];
        uint32_t lastUpdateMs = 0;
    };
    SpectrumData& sharedSpectrum();
    void setSpectrumRange(float startMHz, float stepMHz, int count);
    void sweepOnce();                    // одна развёртка (~50 мс), CC1101 в RX
    void webSpectrum(int band);          // -1 = off, 0..3 = пресет по запросу WebUI
    bool webSpectrumActive();


    // Общий буфер захвата для скриптинга (Lua/JS) — один на всех, не
    // дублируется в каждом движке.
    RawCapture& sharedCapture();
}
