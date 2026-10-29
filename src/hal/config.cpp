#include "config.h"
#include <Preferences.h>

namespace {
    Preferences prefs;

    // Preferences does not like begin()/end() being held open for long —
    // open/close on every operation. A bit slower, but it does not
    // interfere with other code (WebUI etc.) that also touches NVS.
    template <typename F>
    auto withPrefs(bool readOnly, F&& fn) -> decltype(fn()) {
        prefs.begin("megaware", readOnly);
        auto result = fn();
        prefs.end();
        return result;
    }
}

void Config::begin() {
    // NVS by itself does not require explicit initialization from
    // Preferences — kept for API symmetry and in case of future
    // migrations of the settings schema.
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
