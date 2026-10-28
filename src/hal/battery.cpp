#include "battery.h"
#include "../../include/pins.h"
#include <Wire.h>
#include <BQ27220.h>

namespace {
    BQ27220 gauge;
    bool present = false;

    // Топбар читает батарею на КАЖДОМ кадре (percent + isCharging — это
    // две I2C-транзакции), при быстром вращении энкодера — сотни в секунду.
    // Батарея меняется медленно: кэш на 1 секунду, время в кадре дороже.
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
    cacheAt = 0; // первое чтение — сразу, не из кэша
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
