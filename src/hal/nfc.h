#pragma once
#include <Arduino.h>

// =====================================================================
//  Nfc — тонкая обёртка над PN532 (Adafruit_PN532, I2C @0x24).
//  Инициализация ЛЕНИВАЯ (первый ensure() из фичи/скрипта) — если NFC
//  никому не нужен, шина и чип не трогаются вовсе.
//
//  Поддерживается ISO14443A: MIFARE Classic 1K/4K, MIFARE Ultralight,
//  NTAG21x, прочие Type-A по UID. ISO14443B и FeliCa библиотека
//  Adafruit-PN532 не умеет — не заявляем.
//
//  ВАЖНО (общая шина): PN532 делит I2C с BQ27220/Charger — чтение
//  батареи и NFC в один момент времени безопасны (разные адреса).
// =====================================================================

namespace Nfc {
    struct Tag {
        uint8_t uid[10];
        uint8_t uidLen;    // 4, 7 или 10
        char type[32];     // человеческое имя ("MIFARE Classic 1K", "NTAG215", ...)
        bool isClassic;    // MIFARE Classic (дамп с авторизацией)
        bool isNtag;       // NTAG21x/Ultralight (дамп постранично, без ключей)
        uint16_t pages;    // для NTAG/Ultralight: сколько страниц читать
        uint16_t blocks;   // для Classic: 64 (1K) или 256 (4K)
    };

    // Ленивая инициализация (Wire + PN532 + SAMConfig). false — чип не найден.
    bool ensure();
    bool isReady();
    // Есть ли вообще кто-то на шине по адресу PN532 (0x24): отличает
    // «модуля нет» от «модуль есть, но не отвечает».
    bool busPresent();

    // Опрос поля: возвращает true и заполняет out, если в поле есть карта.
    bool readTag(Tag& out);

    // MIFARE Classic: чтение блока с перебором дефолтных ключей.
    // Требует предварительного readTag() этой же карты (UID кэшируется).
    // data — 16 байт; false — ключ не подобран (заполнять можно нулями).
    bool classicReadBlock(uint8_t block, uint8_t* data);
    // Запись блока (та же авторизация, что у чтения). Ключ для сектора
    // берётся из кэша последнего чтения; последняя использованная пара
    // доступна через lastAuthKey().
    bool classicWriteBlock(uint8_t block, const uint8_t* data);
    // Ключ (6 байт), которым открылся последний auth (для карты ключей).
    bool lastAuthKey(uint8_t keyOut[6]);

    // Raw ISO14443-4 APDU к активной карте (после readTag). out ≤ 255.
    bool apdu(const uint8_t* in, int inLen, uint8_t* out, int* outLen);
    // RATS после активации: true у ISO14443-4 карт, ats = historic bytes.
    bool rats(uint8_t* ats, int* atsLen);

    // NTAG/Ultralight: чтение страницы (4 байта), без авторизации.
    // страницы передаются как uint16, чтобы вместить NTAG216 (231 стр.)
    bool ntagReadPage(uint16_t page, uint8_t* data);
    // NTAG/Ultralight: запись страницы (4 байта). Для СВОИХ меток.
    bool ntagWritePage(uint16_t page, const uint8_t* data);

    // Словарь MIFARE-ключей из /rfid/keys.txt (по 12 hex-символов в
    // строке, свои известные ключи; заливается через WebUI). Вызывается
    // перед дампом; дополняет встроенный список дефолтных ключей.
    void loadKeyDictionary();
}
