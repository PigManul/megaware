#include "config.h"
#include <Preferences.h>

namespace {
    Preferences prefs;

    // Preferences не любит держать begin()/end() открытыми надолго —
    // открываем/закрываем на каждую операцию. Чуть медленнее, зато не
    // мешает другим частям кода (WebUI и т.д.), которые тоже лезут в NVS.
    template <typename F>
    auto withPrefs(bool readOnly, F&& fn) -> decltype(fn()) {
        prefs.begin("megaware", readOnly);
        auto result = fn();
        prefs.end();
        return result;
    }
}

void Config::begin() {
    // NVS сама по себе не требует явной инициализации со стороны
    // Preferences — оставлено для симметрии API и на случай будущих
    // миграций схемы настроек.
}

String Config::getString(const char* key, const String& def) {
    return withPrefs(true, [&] { return prefs.getString(key, def); });
}
void Config::setString(const char* key, const String& val) {
    withPrefs(false, [&] { return prefs.putString(key, val); });
}
int Config::getInt(const char* key, int def) {
    return withPrefs(true, [&] { return prefs.getInt(key, def); });
}
void Config::setInt(const char* key, int val) {
    withPrefs(false, [&] { return prefs.putInt(key, val); });
}
bool Config::getBool(const char* key, bool def) {
    return withPrefs(true, [&] { return prefs.getBool(key, def); });
}
void Config::setBool(const char* key, bool val) {
    withPrefs(false, [&] { return prefs.putBool(key, val); });
}
void Config::clearKey(const char* key) {
    withPrefs(false, [&] { return prefs.remove(key); });
}
