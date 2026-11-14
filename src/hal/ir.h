#pragma once
#include <Arduino.h>

// =====================================================================
//  Ir — wrapper over IRremoteESP8266: TX on GPIO2, RX on GPIO1.
//  Lazy initialization; RMT send/recv channels run in parallel.
// =====================================================================

namespace Ir {
    struct Packet {
        char     proto[16];      // "NEC", "RC5", "UNKNOWN", ...
        uint64_t value;          // decoded value (as returned by the lib)
        uint16_t bits;
        uint32_t address;        // if the protocol separates addr/cmd
        uint32_t command;
        uint16_t raw[200];       // timings of the last packet, us (modules,
        int      rawLen;         // signs alternate: +mark, -space)
        uint32_t rawFreq;        // receive carrier (38 kHz)
    };

    bool begin();                              // lazy: irsend.begin + IRIn
    int  protocolCount();                      // number of supported protocols
    const char* protocolName(int idx);
    bool sendByIndex(int protoIdx, uint32_t address, uint32_t command);
    bool poll(Packet& out);                    // true — packet captured
    bool sendParsed(const String& proto, uint32_t address, uint32_t command);
    bool sendValue(const String& proto, uint64_t value, uint16_t bits);
    bool sendRaw(uint32_t hz, const uint16_t* timingsUs, int len);
    bool transmitFile(const String& path);     // /ir/*.ir (Flipper format)

    // Multi-signal .ir files (Flipper format with several "name:" blocks).
    // Signals are parsed skipping '#' comment lines; raw and parsed
    // (protocol/address/command) both supported.
    struct IrSignal {
        char     name[32];
        uint32_t freq;          // carrier Hz (raw only)
        bool     parsed;        // true = protocol/address/command record
        char     proto[16];
        uint32_t address;
        uint32_t command;
        uint16_t raw[500];      // raw timings, us
        int      rawLen;
    };
    int loadSignals(const String& path, IrSignal* out, int max); // count loaded
    bool sendSignal(const IrSignal& sig);
}
