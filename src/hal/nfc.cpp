#include "nfc.h"
#include "../../include/pins.h"
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    // IRQ/RESET на этой плате к I2C-режиму не подключаются: драйвер в
    // I2C-режиме опрашивает RDY-байт по шине (см. isready() в
    // Adafruit_PN532.cpp), пины используются только в GPIO-режимах.
    // -1 -> 255: begin() сам guard'ит _reset, pinMode(255) — no-op ядра.
    Adafruit_PN532 pn532(/*irq*/ -1, /*reset*/ -1, &Wire);

    bool inited = false;
    bool ready = false;

    // UID последней карты — нужен для авторизации секторов Classic
    uint8_t lastUid[10];
    uint8_t lastUidLen = 0;
    bool lastIsClassic = false;
    bool lastIsNtag = false;
    uint16_t lastPages = 0;
    uint16_t lastBlocks = 0;

    // Дефолтные/транспортные ключи MIFARE (перебор сверху вниз)
    const uint8_t KEYS[][6] = {
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // заводской
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, // MAD / NDEF
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // «нулевой»
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
        {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
        {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    };
    constexpr uint8_t KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

    // Кэш авторизации последнего сектора, чтобы не авторизовывать
    // каждый блок заново (auth — самая дорогая операция дампа).
    int8_t authedSector = -1;
    uint8_t authedKeyIdx = 0;
    uint8_t lastKeyBytes[6] = {0};   // ключ, открывший последний auth
    bool lastKeyValid = false;
    void invalidateAuth() { authedSector = -1; lastKeyValid = false; }

    // Пользовательский словарь ключей (/rfid/keys.txt, hex по строке)
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
    Wire.beginTransmission(PN532_I2C_ADDRESS); // 0x24 (7-бит), дефолт библиотеки
    return Wire.endTransmission() == 0;
}

bool ensure() {
    if (inited) return ready;
    inited = true;
    // Явно ставим наши пины I2C: Adafruit_I2CDevice::begin() внутри
    // pn532.begin() дергает Wire.begin() без аргументов, а ядро при
    // уже запущенной шине их сохраняет — но перестрахуемся.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    // ВАЖНО: RST-пульс (GPIO45) ДО проверки шины. Если чип держится в
    // reset (RST float/low), он не ACK-ает — прошлый порядок
    // "busPresent -> решаем" отсекал живой модуль до пробуждения.
    pinMode(PIN_PN532_RST, OUTPUT);
    digitalWrite(PIN_PN532_RST, LOW);
    delay(50);
    digitalWrite(PIN_PN532_RST, HIGH);
    delay(20);
    pinMode(PIN_PN532_IRQ, INPUT);

    pn532.begin();

    // чип может просыпаться после reset — до 5 попыток
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
    // Короткий таймаут: фича опрашивает поле из tick(), висеть нельзя
    if (!pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 60)) return false;
    if (uidLen == 0 || uidLen > 10) return false;

    memset(&out, 0, sizeof(out));
    memcpy(out.uid, uid, uidLen);
    out.uidLen = uidLen;

    // Запоминаем для последующих операций над этой картой
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
        // NTAG/Ultralight различаем по Capability Container (стр. 3)
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

    // Сектор = блок/4 (секторы 0..30), у 4K секторы 32..39 по 16 блоков.
    uint8_t sector = (block < 128) ? block / 4 : 32 + (block - 128) / 16;
    if (sector != authedSector) {
        bool ok = false;
        // 1) встроенные дефолтные/транспортные ключи
        for (uint8_t ki = 0; ki < KEY_COUNT && !ok; ki++) {
            if (pn532.mifareclassic_AuthenticateBlock(lastUid, lastUidLen, block, 0, (uint8_t*)KEYS[ki]) == 1) {
                authedSector = sector;
                authedKeyIdx = ki;
                memcpy(lastKeyBytes, KEYS[ki], 6);
                lastKeyValid = true;
                ok = true;
            }
        }
        // 2) пользовательский словарь (/rfid/keys.txt)
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

bool classicWriteBlock(uint8_t block, const uint8_t* data) {
    if (!ready || !lastIsClassic || lastUidLen == 0) return false;
    uint8_t sector = (block < 128) ? block / 4 : 32 + (block - 128) / 16;
    if (sector != authedSector) {
        // авторизуем тем же перебором (запись=чтение по ключу A)
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
    // RATS (ISO14443-4): первый cmd после активации у совместимых карт
    uint8_t ratsCmd[2] = {0xE0, 0x50};
    uint8_t resp[64];
    uint8_t respLen = 0;
    if (!pn532.inDataExchange(ratsCmd, 2, resp, &respLen)) return false;
    if (respLen < 2 || resp[0] < 1) return false; // ATS TLV
    int n = respLen > 1 ? respLen - 1 : 0;        // без TL-байта
    if (n > *atsLen) n = *atsLen;
    memcpy(ats, resp + 1, n);
    *atsLen = n;
    return true;
}

bool ntagReadPage(uint16_t page, uint8_t* data) {
    if (!ready || !lastIsNtag) return false;
    // NTAG21x-страницы > 255 читаются обычной командой чтения 4-байт страниц
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
