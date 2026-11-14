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
        cachedVoltage = gauge.readVoltageMillivolts() / 1000.0f;

        // charging from the gauge's own CHG flag (BatteryStatus bit 15),
        // NOT from the current sign: the sign convention depends on the
        // gauge configuration and used to show "charging" while idle
        uint16_t st = 0;
        cachedCharging = gauge.readBatteryStatus(st) ? ((st & 0x8000) != 0) : false;

        // sanity gates: an unconfigured fuel gauge reports nonsense
        bool voltSane = cachedVoltage >= 2.5f && cachedVoltage <= 4.6f;
        if (cachedPct < 0 || cachedPct > 100) cachedPct = -1;
        if (!voltSane) { cachedPct = -1; cachedCharging = false; cachedVoltage = 0.0f; }

        // fallback: SOC plainly wrong while voltage is sane - estimate
        // from the LiPo curve so the topbar is never garbage
        float vPct = 0.0f;
        if (voltSane) {
            vPct = (cachedVoltage - 3.30f) / (4.20f - 3.30f) * 100.0f;
            vPct = vPct < 0 ? 0 : (vPct > 100 ? 100 : vPct);
            if (cachedPct < 0) cachedPct = (int)vPct;
            else if (!cachedCharging && cachedPct < (int)vPct / 2) cachedPct = (int)vPct;
        }
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
