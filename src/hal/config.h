#pragma once
#include <Arduino.h>

// =====================================================================
//  Config — единый тонкий слой поверх NVS (Preferences) для ВСЕХ
//  постоянных настроек прошивки. Один namespace ("megaware"), простые
//  типизированные геттеры/сеттеры — специально сделано максимально
//  предсказуемо и однообразно, чтобы новую настройку было легко
//  добавить (в том числе не разбираясь во всём остальном коде):
//
//    Config::setInt("my_new_setting", 42);
//    int v = Config::getInt("my_new_setting", 0);
//
//  Ключи короткие (NVS ограничивает длину ключа 15 символами).
// =====================================================================

namespace Config {
    void begin(); // ничего не грузит сама — просто гарантирует, что NVS доступна

    String getString(const char* key, const String& def);
    void   setString(const char* key, const String& val);
    int    getInt(const char* key, int def);
    void   setInt(const char* key, int val);
    bool   getBool(const char* key, bool def);
    void   setBool(const char* key, bool val);

    void clearKey(const char* key);
}
