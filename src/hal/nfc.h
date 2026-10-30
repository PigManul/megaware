#pragma once
#include <Arduino.h>

// =====================================================================
//  Nfc — thin wrapper over the PN532 (Adafruit_PN532, I2C @0x24).
//  Initialization is LAZY (first ensure() call from a feature/script) —
//  if nobody needs NFC, the bus and the chip are never touched.
//
//  ISO14443A is supported: MIFARE Classic 1K/4K, MIFARE Ultralight,
//  NTAG21x, other Type-A cards by UID. The Adafruit-PN532 library
//  cannot do ISO14443B or FeliCa — not advertised.
//
//  IMPORTANT (shared bus): the PN532 shares I2C with the BQ27220/charger —
//  reading the battery and NFC at the same time is safe (different addresses).
// =====================================================================

namespace Nfc {
    struct Tag {
        uint8_t uid[10];
        uint8_t uidLen;    // 4, 7 or 10
        char type[32];     // human-readable name ("MIFARE Classic 1K", "NTAG215", ...)
        bool isClassic;    // MIFARE Classic (dump with authentication)
        bool isNtag;       // NTAG21x/Ultralight (page-by-page dump, no keys)
        uint16_t pages;    // for NTAG/Ultralight: how many pages to read
        uint16_t blocks;   // for Classic: 64 (1K) or 256 (4K)
    };

    // Lazy initialization (Wire + PN532 + SAMConfig). false — chip not found.
    bool ensure();
    bool isReady();
    // Whether anything at all is on the bus at the PN532 address (0x24):
    // distinguishes "no module" from "module present but not responding".
    bool busPresent();

    // Polls the field: returns true and fills out if a card is in the field.
    bool readTag(Tag& out);

    // MIFARE Classic: reads a block, trying default keys in turn.
    // Requires a prior readTag() of the same card (UID is cached).
    // data is 16 bytes; false — no key found (data can be left as zeros).
    bool classicReadBlock(uint8_t block, uint8_t* data);
    // Writes a block (same authentication as reading). The key for the
    // sector is taken from the cache of the last read; the last key used
    // is available via lastAuthKey().
    bool classicWriteBlock(uint8_t block, const uint8_t* data);
    // The key (6 bytes) that opened the last auth (for the key map).
    bool lastAuthKey(uint8_t keyOut[6]);
    // Authentication test ONLY (no read): true if this key opens the
    // sector containing `block`. Used by the Key Hunter feature.
    bool classicTryKey(uint8_t block, const uint8_t key[6], bool keyA);

    // Raw ISO14443-4 APDU to the active card (after readTag). out ≤ 255.
    bool apdu(const uint8_t* in, int inLen, uint8_t* out, int* outLen);

    // --- Emulation (target mode): the device pretends to be a card ---
    // Starts TGInitAsTarget with the UID/SAK/ATQA from the given Tag.
    // After that the emulation is active — pollEmulate() waits for a
    // reader and returns the received data. Works ONLY with UID-only
    // readers: ISO14443-3A limits FDT to ~86-92 us, while the I2C bus +
    // ESP32 handle commands slower — MIFARE auth/read cannot get through.
    bool emulateStart(const uint8_t* uid, uint8_t uidLen, uint8_t sak);
    bool emulateStop();
    // Returns true if the reader sent data (data/dataLen are filled).
    // Blocks until data is received or the timeout (timeoutMs) expires.
    bool emulatePoll(uint8_t* data, uint8_t* dataLen, uint32_t timeoutMs);
    // Send a response to the reader via setDataTarget.
    bool emulateRespond(const uint8_t* data, uint8_t len);
    // RATS after activation: true for ISO14443-4 cards, ats = historic bytes.
    bool rats(uint8_t* ats, int* atsLen);

    // NTAG/Ultralight: reads a page (4 bytes), no authentication.
    // pages are passed as uint16 to accommodate NTAG216 (231 pages)
    bool ntagReadPage(uint16_t page, uint8_t* data);
    // NTAG/Ultralight: writes a page (4 bytes). For YOUR OWN tags.
    bool ntagWritePage(uint16_t page, const uint8_t* data);

    // MIFARE key dictionary from /rfid/keys.txt (one 12-hex-char key per
    // line, the user's own known keys; uploaded via WebUI). Called before
    // a dump; extends the built-in list of default keys.
    void loadKeyDictionary();
}
