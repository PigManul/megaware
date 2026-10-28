#pragma once
#include <Arduino.h>

// =====================================================================
//  BleScan — общая обёртка над Bluedroid-сканером для фич Bluetooth.
//  ВАЖНО (память): BLE-стек съедает ~60-70 КБ внутреннего heap;
//  сканировать лучше при выключенном Wi-Fi (Settings -> Wi-Fi ->
//  Release Wi-Fi RAM). Фичи сами предупреждают, если памяти мало.
// =====================================================================

namespace BleScan {
    struct Entry {
        char addr[18];
        char name[24];
        int  rssi;
        bool findMy; // Apple Find My / AirTag-подобный advertising
    };

    // Блокирующий скан (seconds). false — BLE не поднялся (мало heap).
    bool scanBlocking(int seconds);
    int  count();
    const Entry& at(int i);
    int  findMyCount();
    // Выгрузить Bluedroid целиком: возвращает ~50-60 КБ heap.
    // Следующий scanBlocking() сам поднимет стек заново (~1 c).
    void deinit();
}
