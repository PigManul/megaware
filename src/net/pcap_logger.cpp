#include "pcap_logger.h"
#include <SD.h>

namespace {
    File pcapFile;
    bool pcapActive = false;
    uint32_t pcapFrames = 0;
    char pcapName[40] = "";

    constexpr size_t BUF_CAP = 8192;
    uint8_t buf[BUF_CAP];
    size_t bufLen = 0;

    void flushBuf() {
        if (bufLen == 0 || !pcapFile) return;
        pcapFile.write(buf, bufLen);
        bufLen = 0;
    }

    void put(const void* p, size_t n) {
        if (bufLen + n > BUF_CAP) flushBuf();
        memcpy(buf + bufLen, p, n);
        bufLen += n;
    }

    void putU32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        put(b, 4);
    }

    uint32_t wallSec() {
        time_t t = time(nullptr);
        return t > 1600000000 ? (uint32_t)t : (uint32_t)(millis() / 1000);
    }
}

bool PcapLogger::start(const String& path) {
    stop();
    if (SD.exists(path)) SD.remove(path);
    pcapFile = SD.open(path, FILE_WRITE);
    if (!pcapFile) return false;
    // pcap global header: magic, v2.4, tz 0, sigfigs 0, snaplen, LINKTYPE_IEEE802_11
    putU32(0xa1b2c3d4);
    putU32(2); putU32(4);
    putU32(0); putU32(0);
    putU32(65535);
    putU32(105);
    flushBuf();
    pcapFrames = 0;
    strncpy(pcapName, path.c_str(), sizeof(pcapName) - 1);
    pcapName[sizeof(pcapName) - 1] = 0; // strncpy may not terminate on a full buffer
    pcapActive = true;
    return true;
}

void PcapLogger::stop() {
    if (!pcapActive) return;
    flushBuf();
    pcapFile.close();
    pcapActive = false;
}

bool PcapLogger::active() { return pcapActive; }
uint32_t PcapLogger::frames() { return pcapFrames; }
const char* PcapLogger::filename() { return pcapName; }

void PcapLogger::writeFrame(const uint8_t* data, int len) {
    if (!pcapActive || !data || len <= 0) return;
    if (len > 2300) len = 2300; // snaplen sanity
    uint32_t us = micros();
    putU32(wallSec());
    putU32(us % 1000000);
    putU32(len + 4);       // incl_len (with fake FCS)
    putU32(len + 4);       // orig_len
    put(data, len);
    static const uint8_t fcs[4] = {0, 0, 0, 0};
    put(fcs, 4);
    pcapFrames++;
    if (bufLen >= BUF_CAP - 2400) flushBuf(); // keep headroom, SD writes are slow
}
