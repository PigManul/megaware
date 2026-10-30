#pragma once
#include <Arduino.h>

// =====================================================================
//  PcapLogger — writes captured 802.11 frames to an SD file in classic
//  pcap format (linktype 105 = IEEE802_11), openable in Wireshark.
//  Frames come from the ESP32 promiscuous callback WITHOUT the FCS,
//  so 4 zero bytes are appended per frame (Wireshark convention).
//  Records are buffered in RAM (8 KB) and flushed to the card to keep
//  up with busy channels.
// =====================================================================

namespace PcapLogger {
    bool start(const String& path); // overwrites, writes the global header
    void stop();
    bool active();
    uint32_t frames();
    const char* filename();
    void writeFrame(const uint8_t* data, int len); // one 802.11 frame, no FCS
}
