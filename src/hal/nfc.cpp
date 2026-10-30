#include "nfc.h"
#include "../../include/pins.h"
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    // IRQ/RESET are not wired to the I2C mode on this board: in I2C mode
    // the driver polls the RDY byte over the bus (see isready() in
    // Adafruit_PN532.cpp); the pins are only used in GPIO modes.
    // -1 -> 255: begin() guards _reset itself, pinMode(255) is a core no-op.
    Adafruit_PN532 pn532(/*irq*/ -1, /*reset*/ -1, &Wire);

    bool inited = false;
    bool ready = false;

    // UID of the last card — needed to authenticate Classic sectors
    uint8_t lastUid[10];
    uint8_t lastUidLen = 0;
    bool lastIsClassic = false;
    bool lastIsNtag = false;
    uint16_t lastPages = 0;
    uint16_t lastBlocks = 0;

    // Default/transport MIFARE keys (tried top to bottom)
    const uint8_t KEYS[][6] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // factory
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, // MAD / NDEF
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // "all zeros"
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
        {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
        {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    };
    constexpr uint8_t KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

    // Cache of the last authenticated sector so each block does not have
    // to be authenticated again (auth is the most expensive dump operation).
    int8_t authedSector = -1;
    uint8_t authedKeyIdx = 0;
    uint8_t lastKeyBytes[6] = {0};   // key that opened the last auth
    bool lastKeyValid = false;
    void invalidateAuth() { authedSector = -1; lastKeyValid = false; }

    // User key dictionary (/rfid/keys.txt, one hex key per line)
    constexpr int MAX_EXTRA_KEYS = 24;
    uint8_t extraKeys[MAX_EXTRA_KEYS][6];
    int extraKeyCount = 0;

    bool hexKeyToBytes(const char* s, uint8_t out[6]) {
        if (strlen(s) < 12) return false;
        for (int i = 0; i < 6; i++) {
            char byteStr[3] = {s[i * 2], s[i * 2 + 1], 0};
            out[i] = (uint8_t)strtol(byteStr, nullptr, 16);
        }
        return true;
    }
}

namespace Nfc {

bool busPresent() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.beginTransmission(PN532_I2C_ADDRESS); // 0x24 (7-bit), library default
    return Wire.endTransmission() == 0;
}

bool ensure() {
    if (inited) return ready;
    inited = true;
    // Explicitly set our I2C pins: Adafruit_I2CDevice::begin() inside
    // pn532.begin() calls Wire.begin() without arguments, and the core
    // keeps them when the bus is already running — but play it safe.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    // IMPORTANT: RST pulse (GPIO45) BEFORE checking the bus. If the chip
    // is held in reset (RST float/low), it does not ACK — the previous
    // "busPresent -> decide" order discarded a live module before wakeup.
    pinMode(PIN_PN532_RST, OUTPUT);
    digitalWrite(PIN_PN532_RST, LOW);
    delay(50);
    digitalWrite(PIN_PN532_RST, HIGH);
    delay(20);
    pinMode(PIN_PN532_IRQ, INPUT);

    pn532.begin();

    // the chip may still be waking up after reset — up to 5 attempts
    uint32_t ver = 0;
    for (int attempt = 0; attempt < 5 && !ver; attempt++) {
        ver = pn532.getFirmwareVersion();
        if (!ver) delay(100);
    }
    if (!ver) { ready = false; return false; }
    ready = pn532.SAMConfig();
    return ready;
}

bool isReady() { return inited && ready; }

bool readTag(Tag& out) {
    if (!ensure()) return false;

    uint8_t uid[10];
    uint8_t uidLen = 0;
    // Short timeout: the feature polls the field from tick(), must not hang
    if (!pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 60)) return false;
    if (uidLen == 0 || uidLen > 10) return false;

    memset(&out, 0, sizeof(out));
    memcpy(out.uid, uid, uidLen);
    out.uidLen = uidLen;

    // Remember for subsequent operations on this card
    memcpy(lastUid, uid, uidLen);
    lastUidLen = uidLen;
    invalidateAuth();

    if (uidLen == 4) {
        const uint8_t* k = KEYS[0];
        bool classic1k = pn532.mifareclassic_AuthenticateBlock(uid, uidLen, 4, 0, (uint8_t*)k) == 1;
        if (classic1k) {
            bool classic4k = pn532.mifareclassic_AuthenticateBlock(uid, uidLen, 128, 0, (uint8_t*)k) == 1;
            out.isClassic = true;
            out.blocks = classic4k ? 256 : 64;
            snprintf(out.type, sizeof(out.type), "MIFARE Classic %s", classic4k ? "4K" : "1K");
        } else {
            snprintf(out.type, sizeof(out.type), "ISO14443A (4B, key unknown)");
        }
    } else if (uidLen == 7) {
        // NTAG/Ultralight are told apart by the Capability Container (page 3)
        uint8_t page[4];
        if (pn532.ntag2xx_ReadPage(3, page) == 1 && page[0] == 0xE1) {
            out.isNtag = true;
            switch (page[2]) {
                case 0x12: out.pages = 45;  snprintf(out.type, sizeof(out.type), "NTAG213"); break;
                case 0x3E: out.pages = 135; snprintf(out.type, sizeof(out.type), "NTAG215"); break;
                case 0x6D: out.pages = 231; snprintf(out.type, sizeof(out.type), "NTAG216"); break;
                default:   out.pages = 16;  snprintf(out.type, sizeof(out.type), "MIFARE Ultralight"); break;
            }
        } else {
            snprintf(out.type, sizeof(out.type), "ISO14443A (7B)");
        }
    } else {
        snprintf(out.type, sizeof(out.type), "ISO14443A (%dB UID)", uidLen);
    }

    lastIsClassic = out.isClassic;
    lastIsNtag = out.isNtag;
    lastPages = out.pages;
    lastBlocks = out.blocks;
    return true;
}

bool classicReadBlock(uint8_t block, uint8_t* data) {
    if (!ready || !lastIsClassic || lastUidLen == 0) return false;

    // Sector = block/4 (sectors 0..30); in 4K, sectors 32..39 hold 16 blocks each.
    uint8_t sector = (block < 128) ? block / 4 : 32 + (block - 128) / 16;
    if (sector != authedSector) {
        bool ok = false;
        // 1) built-in default/transport keys
        for (uint8_t ki = 0; ki < KEY_COUNT && !ok; ki++) {
            if (pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block, 0, (uint8_t*)KEYS[ki]) == 1) {
                authedSector = sector;
                authedKeyIdx = ki;
                memcpy(lastKeyBytes, KEYS[ki], 6);
                lastKeyValid = true;
                ok = true;
            }
        }
        // 2) user dictionary (/rfid/keys.txt)
        for (int ki = 0; ki < extraKeyCount && !ok; ki++) {
            if (pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block, 0, extraKeys[ki]) == 1) {
                authedSector = sector;
                memcpy(lastKeyBytes, extraKeys[ki], 6);
                lastKeyValid = true;
                ok = true;
            }
        }
        if (!ok) return false;
    }
    return pn532.mifareclassic_ReadDataBlock(block, data) == 1;
}

bool lastAuthKey(uint8_t keyOut[6]) {
    if (!lastKeyValid) return false;
    memcpy(keyOut, lastKeyBytes, 6);
    return true;
}

bool classicTryKey(uint8_t block, const uint8_t key[6], bool keyA) {
    if (!ready || !lastIsClassic || lastUidLen == 0) return false;
    // 0 = KEY A, 1 = KEY B (Adafruit-PN532 convention)
    invalidateAuth(); // the sector key cache is no longer trustworthy
    return pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block,
                                                 keyA ? 0 : 1, (uint8_t*)key) == 1;
}

bool classicWriteBlock(uint8_t block, const uint8_t* data) {
    if (!ready || !lastIsClassic || lastUidLen == 0) return false;
    uint8_t sector = (block < 128) ? block / 4 : 32 + (block - 128) / 16;
    if (sector != authedSector) {
        // authenticate with the same key trial loop (write = read via key A)
        bool ok = false;
        for (uint8_t ki = 0; ki < KEY_COUNT && !ok; ki++) {
            if (pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block, 0, (uint8_t*)KEYS[ki]) == 1) {
                authedSector = sector;
                memcpy(lastKeyBytes, KEYS[ki], 6);
                lastKeyValid = true;
                ok = true;
            }
        }
        for (int ki = 0; ki < extraKeyCount && !ok; ki++) {
            if (pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block, 0, extraKeys[ki]) == 1) {
                authedSector = sector;
                memcpy(lastKeyBytes, extraKeys[ki], 6);
                lastKeyValid = true;
                ok = true;
            }
        }
        if (!ok) return false;
    }
    uint8_t buf[16];
    memcpy(buf, data, 16);
    return pn532.mifareclassic_WriteDataBlock(block, buf) == 1;
}

bool apdu(const uint8_t* in, int inLen, uint8_t* out, int* outLen) {
    if (!ready || lastUidLen == 0) return false;
    uint8_t resp[255];
    uint8_t respLen = 0;
    uint8_t buf[64];
    if (inLen <= 0 || inLen > 64) return false;
    memcpy(buf, in, inLen);
    if (!pn532.inDataExchange(buf, (uint8_t)inLen, resp, &respLen)) return false;
    int n = respLen > *outLen ? *outLen : respLen;
    memcpy(out, resp, n);
    *outLen = n;
    return true;
}

bool rats(uint8_t* ats, int* atsLen) {
    if (!ready || lastUidLen == 0) return false;
    // RATS (ISO14443-4): the first cmd after activation on compatible cards
    uint8_t ratsCmd[2] = {0xE0, 0x50};
    uint8_t resp[64];
    uint8_t respLen = 0;
    if (!pn532.inDataExchange(ratsCmd, 2, resp, &respLen)) return false;
    if (respLen < 2 || resp[0] < 1) return false; // ATS TLV
    int n = respLen > 1 ? respLen - 1 : 0;        // without the TL byte
    if (n > *atsLen) n = *atsLen;
    memcpy(ats, resp + 1, n);
    *atsLen = n;
    return true;
}

// --- Emulation: PN532 in target mode ---

bool emulateStart(const uint8_t* uid, uint8_t uidLen, uint8_t sak) {
    if (!ready) return false;

    // TGInitAsTarget: the PN532 would wait for an external field and reply
    // with its UID. Command format (PN532 User Manual, rev. 8, section 7.3.2):
    // [0x8C] mode(1) MifareParams(6) FelicaParams(18) NFCID3(10) len(1) genBytes(N)
    // mode 0x04 = PICC only, passive, MIFARE compatible

    uint8_t cmd[40];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x8C;                      // TGInitAsTarget
    cmd[1] = 0x04;                      // mode: PICC only, passive, MIFARE
    // MifareParams (6 bytes): CUID[4], SAK, ATQA[2]
    cmd[2] = uid[0];                    // CUID byte 0
    cmd[3] = uidLen > 1 ? uid[1] : 0;
    cmd[4] = uidLen > 2 ? uid[2] : 0;
    cmd[5] = uidLen > 3 ? uid[3] : 0;
    cmd[6] = sak;                       // SAK
    cmd[7] = 0x04;                      // ATQA LSB (4-byte UID → ATQA 0x0004)
    cmd[8] = 0x00;                      // ATQA MSB
    // FelicaParams (18 bytes) — zeros for Type A
    // NFCID3 (10 bytes) — zeros
    // General bytes length = 0
    // Total size: 1(cmd) + 1(mode) + 6 + 18 + 10 + 1(len) = 37

    // Send via sendCommandCheckAck (public Adafruit method)
    // Command 0x8C — TGInitAsTarget
    // sendCommandCheckAck adds the 0xD4 prefix itself

    // Using sendCommandCheckAck directly:
    uint8_t tgCmd[37];
    tgCmd[0] = 0x8C;
    memcpy(tgCmd + 1, cmd + 1, 36);

    if (!pn532.sendCommandCheckAck(tgCmd, 37, 1000)) return false;

    // PN532 is now in target mode, waiting for the reader field
    return true;
}

bool emulateStop() {
    // Exiting target mode: FACTORYRESET or simply SAMConfig again
    pn532.SAMConfig();
    return true;
}

bool emulatePoll(uint8_t* data, uint8_t* dataLen, uint32_t timeoutMs) {
    // getDataTarget: the PN532 hands over the data received from the reader
    // In the Adafruit lib this is getDataTarget(uint8_t* cmd, uint8_t* cmdlen)
    uint8_t tmp[64];
    uint8_t tmpLen = 0;
    if (!pn532.getDataTarget(tmp, &tmpLen)) return false;
    if (tmpLen == 0) return false;
    int n = tmpLen > *dataLen ? *dataLen : tmpLen;
    memcpy(data, tmp, n);
    *dataLen = n;
    return true;
}

bool emulateRespond(const uint8_t* data, uint8_t len) {
    return pn532.setDataTarget(const_cast<uint8_t*>(data), len);
}

bool ntagReadPage(uint16_t page, uint8_t* data) {
    if (!ready || !lastIsNtag) return false;
    // NTAG21x pages > 255 are read with the regular 4-byte page read command
    return pn532.mifareultralight_ReadPage((uint8_t)page, data) == 1 ||
           pn532.ntag2xx_ReadPage((uint8_t)page, data) == 1;
}

bool ntagWritePage(uint16_t page, const uint8_t* data) {
    if (!ready || !lastIsNtag) return false;
    uint8_t buf[4];
    memcpy(buf, data, 4);
    return pn532.ntag2xx_WritePage((uint8_t)page, buf) == 1;
}

void loadKeyDictionary() {
    extraKeyCount = 0;
    File f = SD.exists("/rfid/keys.txt") ? SD.open("/rfid/keys.txt")
                                         : LittleFS.open("/rfid/keys.txt");
    if (!f) return;
    while (f.available() && extraKeyCount < MAX_EXTRA_KEYS) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("#") || line.length() == 0) continue;
        if (hexKeyToBytes(line.c_str(), extraKeys[extraKeyCount])) extraKeyCount++;
    }
    f.close();
    invalidateAuth();
}

} // namespace Nfc
