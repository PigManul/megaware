#include "ir.h"
#include "../../include/pins.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    IRsend irsend(PIN_IR_TX);
    IRrecv irrecv(PIN_IR_RX, 250); // raw timing buffer (for long AC remotes)
    decode_results results;
    bool inited = false;
    bool recvOn = false;

    void ensureRecv() {
        if (!recvOn) { irrecv.enableIRIn(); recvOn = true; }
    }

}

namespace Ir {

bool begin() {
    if (inited) return true;
    irsend.begin();
    ensureRecv();
    inited = true;
    return true;
}

    // The IRremoteESP8266 senders take a single 64-bit value
    // (except Panasonic: addr16 + data32). Flipper addresses are combined:
    // value = addr16 | cmd16<<16 — our Capture files round-trip 1:1.
    bool sendParsed(const String& proto, uint32_t address, uint32_t command) {
        if (proto == "PANASONIC") {
            irsend.sendPanasonic((uint16_t)address, command);
            return true;
        }
        uint64_t v = ((uint64_t)(address & 0xFFFF)) | (((uint64_t)(command & 0xFFFF)) << 16);
        return Ir::sendValue(proto, v, 0);
    }

    uint32_t parseHexBytes(const String& s) { // "00 00 00 80" -> 0x00000080
        uint32_t v = 0;
        int start = 0;
        while (start < (int)s.length()) {
            while (start < (int)s.length() && s[start] == ' ') start++;
            if (start >= (int)s.length()) break;
            int end = s.indexOf(' ', start);
            if (end < 0) end = s.length();
            v = (v << 8) | (uint32_t)strtol(s.substring(start, end).c_str(), nullptr, 16);
            start = end + 1;
        }
        return v;
    }
bool poll(Packet& out) {
    begin();
    ensureRecv();
    if (!irrecv.decode(&results)) return false;
    irrecv.resume();

    memset(&out, 0, sizeof(out));
    snprintf(out.proto, sizeof(out.proto), "%s", typeToString(results.decode_type, false).c_str());
    out.value = results.value;
    out.bits = results.bits;
    out.address = results.address;
    out.command = results.command;
    out.rawLen = 0;
    out.rawFreq = 38000;
    for (uint16_t i = 1; i < results.rawlen && out.rawLen < 200; i++) {
        uint32_t us = (uint32_t)results.rawbuf[i] * RAWTICK; // RAWTICK = 50 us
        if (us > 65000) us = 65000;
        out.raw[out.rawLen++] = (uint16_t)us;
    }
    return true;
}

static const char* const PROTOS[] = {
    "NEC", "SONY", "RC5", "RC6", "SAMSUNG", "LG", "PANASONIC", "JVC",
};
constexpr int PROTO_COUNT = sizeof(PROTOS) / sizeof(PROTOS[0]);

int protocolCount() { return PROTO_COUNT; }
const char* protocolName(int idx) {
    if (idx < 0 || idx >= PROTO_COUNT) return "?";
    return PROTOS[idx];
}

bool sendByIndex(int protoIdx, uint32_t address, uint32_t command) {
    if (protoIdx < 0 || protoIdx >= PROTO_COUNT) return false;
    return sendParsed(PROTOS[protoIdx], address, command);
}

bool sendValue(const String& proto, uint64_t value, uint16_t bits) {
    begin();
    if (proto == "NEC")       { irsend.sendNEC(value, bits ? bits : 32); return true; }
    if (proto == "SONY")      { irsend.sendSony(value, bits ? bits : 20, 2); return true; }
    if (proto == "RC5")       { irsend.sendRC5(value, bits ? bits : 12); return true; }
    if (proto == "RC6")       { irsend.sendRC6(value, bits ? bits : 20); return true; }
    if (proto == "SAMSUNG")   { irsend.sendSAMSUNG(value, bits ? bits : 32); return true; }
    if (proto == "LG")        { irsend.sendLG(value, bits ? bits : 28); return true; }
    if (proto == "JVC")       { irsend.sendJVC(value, bits ? bits : 16); return true; }
    if (proto == "PANASONIC") { irsend.sendPanasonic((uint16_t)(value >> 32), (uint32_t)(value & 0xFFFFFFFF)); return true; }
    return false;
}

bool sendRaw(uint32_t hz, const uint16_t* timingsUs, int len) {
    begin();
    if (len <= 0 || len > 400 || hz < 20000 || hz > 60000) return false;
    irsend.sendRaw(const_cast<uint16_t*>(timingsUs), len, hz);
    ensureRecv();
    return true;
}

bool transmitFile(const String& path) {
    File f = SD.exists(path) ? SD.open(path) : LittleFS.open(path);
    if (!f) return false;

    String type = "", proto = "", addrStr = "", cmdStr = "", dataStr = "";
    uint32_t freq = 38000;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int colon = line.indexOf(':');
        if (colon <= 0) continue;
        String key = line.substring(0, colon);
        key.trim();
        String val = line.substring(colon + 1);
        val.trim();
        if (key == "type") type = val;
        else if (key == "protocol") proto = val;
        else if (key == "address") addrStr = val;
        else if (key == "command") cmdStr = val;
        else if (key == "frequency") freq = (uint32_t)val.toFloat();
        else if (key == "data") dataStr = val;
    }
    f.close();

    if (type == "parsed") {
        if (proto == "PANASONIC") {
            // address: 2 bytes, command: 4 bytes
            uint32_t addr = parseHexBytes(addrStr) & 0xFFFF;
            uint32_t cmd = parseHexBytes(cmdStr);
            return sendParsed(proto, addr, cmd);
        }
        return sendParsed(proto, parseHexBytes(addrStr), parseHexBytes(cmdStr));
    }
    if (type == "raw") {
        uint16_t t[400];
        int n = 0;
        int start = 0;
        while (start < (int)dataStr.length() && n < 400) {
            while (start < (int)dataStr.length() && dataStr[start] == ' ') start++;
            if (start >= (int)dataStr.length()) break;
            int end = dataStr.indexOf(' ', start);
            if (end < 0) end = dataStr.length();
            long v = dataStr.substring(start, end).toInt();
            start = end + 1;
            if (v < 0) v = -v;
            if (v == 0) continue;
            if (v > 65000) v = 65000;
            t[n++] = (uint16_t)v;
        }
        return sendRaw(freq, t, n);
    }
    return false;
}

} // namespace Ir
