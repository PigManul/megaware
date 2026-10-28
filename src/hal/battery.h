#pragma once
#include <Arduino.h>

namespace Battery {
    bool begin();
    bool isPresent();
    int   percent();
    float voltage();
    bool  isCharging();
}
