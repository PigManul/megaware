#pragma once
#include <Arduino.h>

// =====================================================================
//  Config — a single thin layer over NVS (Preferences) for ALL
//  persistent firmware settings. One namespace ("megaware"), simple
//  typed getters/setters — deliberately made as predictable and
//  uniform as possible so that a new setting is easy to add
//  (even without understanding the rest of the code):
//
//    Config::setInt("my_new_setting", 42);
//    int v = Config::getInt("my_new_setting", 0);
//
//  Keys are short (NVS limits the key length to 15 characters).
// =====================================================================

namespace Config {
    void begin(); // loads nothing itself — just guarantees that NVS is available

    String getString(const char* key, const String& def);
    void   setString(const char* key, const String& val);
    int    getInt(const char* key, int def);
    void   setInt(const char* key, int val);
    bool   getBool(const char* key, bool def);
    void   setBool(const char* key, bool val);

    void clearKey(const char* key);
}
