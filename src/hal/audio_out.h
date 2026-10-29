#pragma once
#include <Arduino.h>

// =====================================================================
//  AudioOut — NS4168 speaker via I2S TX: BCLK=46, LRCLK=40, DOUT=7.
//  The output takes an int16 stereo stream (mono is duplicated by us).
//  46/40 are strapping pins: configuration happens after boot,
//  it does not affect the boot latch.
// =====================================================================

namespace AudioOut {
    bool begin(uint32_t sampleRateHz); // (re)configuration for the file
    bool write(const int16_t* samples, int count); // count values (stereo-interleaved)
    void stop();                       // silence (driver stays installed)
    bool active();
}
