#include "battery.h"
#include "../../include/pins.h"
#include <Wire.h>
#include <BQ27220.h>

namespace {
    BQ27220 gauge;
    bool present = false;

    // The topbar reads the battery on EVERY frame (percent + isCharging are
    // two I2C transactions); with fast encoder rotation that is hundreds
    // per second. The battery changes slowly: 1-second cache, frame time matters more.
    constexpr uint32_t CACHE_MS = 1000;
    uint32_t cacheAt = 0;
    int cachedPct = -1;
    float cachedVoltage = 0.0f;
    bool cachedCharging = false;

    void refresh() {
        uint32_t now = millis();
        if (present && now - cacheAt < CACHE_MS) return;
        cacheAt = now;
        if (!present) { cachedPct = -1; cachedVoltage = 0.0f; cachedCharging = false; return; }
        cachedPct = gauge.readStateOfChargePercent();
        if (cachedPct < 0 || cachedPct > 100) cachedPct = -1;
        cachedVoltage = gauge.readVoltageMillivolts() / 1000.0f;
        cachedCharging = gauge.readCurrentMilliamps() > 0;
    }
}

bool Battery::begin() {
    present = gauge.begin(Wire, 0x55, PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    cacheAt = 0; // first read — immediately, not from cache
    return present;
}

bool Battery::isPresent() { return present; }

int Battery::percent() {
    refresh();
    return cachedPct;
}

float Battery::voltage() {
    refresh();
    return cachedVoltage;
}

bool Battery::isCharging() {
    refresh();
    return cachedCharging;
}
