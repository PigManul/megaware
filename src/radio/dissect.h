#pragma once
#include <Arduino.h>
#include "subghz.h"

// =====================================================================
//  Dissect — Sub-GHz protocol dissector ("mini-rtl_433 on device").
//  Turns a raw edge capture (RawCapture from CC1101 GDO2) into a
//  human-readable packet: encoding autodetect (PWM/PPM vs Manchester
//  vs NRZ), timing unit / baud estimate, preamble + sync extraction,
//  bit-level payload and a match against a small database of the most
//  common remotes (CAME/Nice 12-bit, Princeton PT-2262 with tri-state
//  digits). Unknown protocols still get a full field view instead of
//  raw hex garbage.
//
//  For PWM/PPM frames the digit stream is the primary representation:
//  one digit = one hi/lo pulse pair, values 0 (short hi, long lo),
//  1 (long hi, short lo), 2 = tri-state "F" (equal halves, PT-2262
//  open pin). 12 digits no-F -> CAME/Nice-class gate remote; 12 digits
//  with F -> PT-2262 address+buttons.
//
//  NOTE: this is a DECODER, not a decryptor. Rolling-code payloads
//  (KeeLoq etc.) are shown as opaque encrypted blocks by design.
// =====================================================================

struct DissectResult {
    bool valid = false;
    int enc = 3;                 // 0 PWM/PPM, 1 Manchester, 2 NRZ, 3 unknown
    const char* encName = "";
    uint32_t unitUs = 0;         // shortest meaningful pulse (timing unit)
    uint32_t baud = 0;           // bit rate estimate
    int bitCount = 0;            // decoded bits (2 per digit for PPM)
    uint8_t bits[96];            // up to 768 bits, MSB first
    // PPM digit stream (one digit per pulse pair)
    int digitCount = 0;
    uint8_t digits[144];         // 0, 1 or 2 (= tri-state F)
    int preambleBits = 0;        // leading alternating 0/1 run length (bits)
    uint16_t sync = 0;           // 16-bit sync word after the preamble (0 = none)
    float ratio = 0;             // measured long/short pulse ratio (PPM)
    bool longGap = false;        // gap run >= 8 units (EV1527-style framing)
    bool dewhitened = false;     // PN9 de-scrambling applied (bytes XORed)
    const char* proto = "unknown";
    char summary[160] = "";      // human-readable decode
};

namespace Dissect {
    bool analyze(const RawCapture& cap, DissectResult& out);
    // bits as hex into buf (up to 64 bytes = 128 chars)
    void hexOf(const DissectResult& r, char* buf, size_t bufsz);
    // XOR the decoded byte stream with the CC1101 PN9 keystream
    // (seed 0x1FF, poly x^9+x^5+1, LSB-first per byte) - reverses
    // hardware whitening used by BLE-like Sub-GHz senders.
    void dewhitenPn9(DissectResult& r);
}
