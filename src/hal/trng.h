#pragma once
#include <Arduino.h>
// True RNG — ESP32-S3 hardware generator (RF noise, not pseudo)
namespace Trng {
    uint32_t rng32();
    void rngBytes(uint8_t* out, int len);
}
