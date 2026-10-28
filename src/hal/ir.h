#pragma once
#include <Arduino.h>

// =====================================================================
//  Ir — обёртка над IRremoteESP8266: TX на GPIO2, RX на GPIO1.
//  Ленивая инициализация; send/recv каналы RMT живут параллельно.
// =====================================================================

namespace Ir {
    struct Packet {
        char     proto[16];      // "NEC", "RC5", "UNKNOWN", ...
        uint64_t value;          // декодированное значение (как даёт lib)
        uint16_t bits;
        uint32_t address;        // если протокол разделяет addr/cmd
        uint32_t command;
        uint16_t raw[200];       // тайминги последнего пакета, мкс (модули,
        int      rawLen;         // знаки чередуются: +mark, -space)
        uint32_t rawFreq;        // несущая приёма (38 кГц)
    };

    bool begin();                              // ленивая: irsend.begin + IRIn
    bool poll(Packet& out);                    // true — поймали пакет
    bool sendParsed(const String& proto, uint32_t address, uint32_t command);
    bool sendValue(const String& proto, uint64_t value, uint16_t bits);
    bool sendRaw(uint32_t hz, const uint16_t* timingsUs, int len);
    bool transmitFile(const String& path);     // /ir/*.ir (Flipper-формат)
}
