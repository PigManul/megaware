#include "lua_engine.h"
#include "../hal/board.h"
#include "../hal/battery.h"
#include "../hal/config.h"
#include "../hal/nfc.h"
#include "../radio/subghz.h"
#include "../net/wifi_manager.h"
#include "game_io.h"
#include <esp_system.h>
#include <SD.h>
#include <LittleFS.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace {
    String gLastError;

    File openForRead(const String& path) {
        File f = SD.open(path);
        if (f) return f;
        return LittleFS.open(path);
    }
    File openForWrite(const String& path) {
        File f = SD.open(path, FILE_WRITE);
        if (f) return f;
        return LittleFS.open(path, FILE_WRITE);
    }

    int lua_sleep(lua_State* L) {
        long ms = luaL_checkinteger(L, 1);
        uint32_t start = millis();
        while ((long)(millis() - start) < ms) {
            Board::updateLeds(millis());
            InputEvent ev = GameIO::poll();
            if (ev == InputEvent::UserKey) {
                return luaL_error(L, "script aborted (Quit pressed)");
            }
            delay(10);
        }
        return 0;
    }

    int lua_print_bridge(lua_State* L) {
        int n = lua_gettop(L);
        for (int i = 1; i <= n; i++) {
            const char* s = lua_tostring(L, i);
            if (s) { Serial.print(s); Serial.print(' '); }
        }
        Serial.println();
        return 0;
    }

    int lua_led_setColor(lua_State* L) {
        Board::setLedColor((uint8_t)luaL_checkinteger(L, 1), (uint8_t)luaL_checkinteger(L, 2), (uint8_t)luaL_checkinteger(L, 3));
        return 0;
    }
    int lua_led_setMode(lua_State* L) {
        Board::setLedMode((LedMode)(int)luaL_checkinteger(L, 1));
        return 0;
    }
    int lua_led_setBrightness(lua_State* L) {
        Board::setLedBrightness((uint8_t)constrain((int)luaL_checkinteger(L, 1), 0, 255));
        return 0;
    }

    int lua_gfx_clear(lua_State* L) {
        GameIO::clear((uint8_t)luaL_checkinteger(L, 1), (uint8_t)luaL_checkinteger(L, 2), (uint8_t)luaL_checkinteger(L, 3));
        return 0;
    }
    int lua_gfx_rect(lua_State* L) {
        GameIO::rect((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                     (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                     (uint8_t)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6), (uint8_t)luaL_checkinteger(L, 7));
        return 0;
    }
    int lua_gfx_circle(lua_State* L) {
        GameIO::circle((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                       (uint8_t)luaL_checkinteger(L, 4), (uint8_t)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        return 0;
    }
    int lua_gfx_text(lua_State* L) {
        GameIO::text((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), luaL_checkstring(L, 3),
                     (uint8_t)luaL_checkinteger(L, 4), (uint8_t)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        return 0;
    }
    int lua_gfx_width(lua_State* L)  { lua_pushinteger(L, GameIO::screenWidth());  return 1; }
    int lua_gfx_height(lua_State* L) { lua_pushinteger(L, GameIO::screenHeight()); return 1; }

    int lua_input_update(lua_State* L) { GameIO::poll(); return 0; }
    int lua_input_dx(lua_State* L)     { lua_pushinteger(L, GameIO::dx()); return 1; }
    int lua_input_click(lua_State* L)  { lua_pushboolean(L, GameIO::click()); return 1; }
    int lua_input_quit(lua_State* L)   { lua_pushboolean(L, GameIO::quit()); return 1; }

    int lua_subghz_isReady(lua_State* L)   { lua_pushboolean(L, SubGhz::isReady()); return 1; }
    int lua_subghz_frequency(lua_State* L) { lua_pushnumber(L, SubGhz::currentFrequencyMHz()); return 1; }
    int lua_subghz_setBand(lua_State* L) {
        int b = (int)luaL_checkinteger(L, 1);
        SubGhz::setBand(b == 0 ? SubGhzBand::MHz315 : (b == 2 ? SubGhzBand::MHz868 : SubGhzBand::MHz433));
        return 0;
    }
    int lua_subghz_rssi(lua_State* L) { lua_pushnumber(L, SubGhz::readRssiDbm()); return 1; }
    int lua_subghz_startCapture(lua_State* L) {
        long ms = luaL_optinteger(L, 1, 10000);
        SubGhz::startRawCapture(&SubGhz::sharedCapture(), (uint32_t)ms);
        return 0;
    }
    int lua_subghz_isCapturing(lua_State* L) { lua_pushboolean(L, SubGhz::isCapturing()); return 1; }
    int lua_subghz_stopCapture(lua_State* L) { SubGhz::stopRawCapture(); return 0; }
    int lua_subghz_captureCount(lua_State* L) { lua_pushinteger(L, (int)SubGhz::sharedCapture().count); return 1; }
    int lua_subghz_replay(lua_State* L) { lua_pushboolean(L, SubGhz::replayRaw(SubGhz::sharedCapture())); return 1; }
    int lua_subghz_transmitFile(lua_State* L) {
        lua_pushboolean(L, SubGhz::transmitFile(luaL_checkstring(L, 1)));
        return 1;
    }
    int lua_subghz_transmit(lua_State* L) {
        long hz = luaL_checkinteger(L, 1);
        lua_pushboolean(L, SubGhz::transmitRaw((uint32_t)hz, luaL_checkstring(L, 2)));
        return 1;
    }

    int lua_wifi_isConnected(lua_State* L) { lua_pushboolean(L, WifiMgr::isConnected()); return 1; }
    int lua_wifi_ssid(lua_State* L)        { lua_pushstring(L, WifiMgr::currentSSID().c_str()); return 1; }

    int lua_battery_percent(lua_State* L)  { lua_pushinteger(L, Battery::percent()); return 1; }
    int lua_battery_voltage(lua_State* L)  { lua_pushnumber(L, Battery::voltage()); return 1; }
    int lua_battery_charging(lua_State* L) { lua_pushboolean(L, Battery::isCharging()); return 1; }

    int lua_nfc_isReady(lua_State* L) { lua_pushboolean(L, Nfc::ensure()); return 1; }
    int lua_nfc_uid(lua_State* L) {
        Nfc::Tag t;
        if (!Nfc::readTag(t)) { lua_pushnil(L); return 1; }
        char buf[32];
        size_t pos = 0;
        for (uint8_t i = 0; i < t.uidLen && pos + 2 < sizeof(buf); i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", t.uid[i]);
        lua_pushstring(L, buf);
        return 1;
    }
    int lua_nfc_type(lua_State* L) {
        Nfc::Tag t;
        if (!Nfc::readTag(t)) { lua_pushnil(L); return 1; }
        lua_pushstring(L, t.type);
        return 1;
    }

    int lua_config_getInt(lua_State* L) {
        lua_pushinteger(L, Config::getInt(luaL_checkstring(L, 1), (int)luaL_optinteger(L, 2, 0)));
        return 1;
    }
    int lua_config_setInt(lua_State* L) {
        Config::setInt(luaL_checkstring(L, 1), (int)luaL_checkinteger(L, 2));
        return 0;
    }
    int lua_config_getStr(lua_State* L) {
        String v = Config::getString(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""));
        lua_pushstring(L, v.c_str());
        return 1;
    }
    int lua_config_setStr(lua_State* L) {
        Config::setString(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
        return 0;
    }

    int lua_fs_read(lua_State* L) {
        File f = openForRead(luaL_checkstring(L, 1));
        if (!f) { lua_pushnil(L); return 1; }
        String s = f.readString();
        f.close();
        lua_pushstring(L, s.c_str());
        return 1;
    }
    int lua_fs_write(lua_State* L) {
        File f = openForWrite(luaL_checkstring(L, 1));
        if (!f) { lua_pushboolean(L, false); return 1; }
        f.print(luaL_checkstring(L, 2));
        f.close();
        lua_pushboolean(L, true);
        return 1;
    }
    int lua_fs_exists(lua_State* L) {
        const char* p = luaL_checkstring(L, 1);
        lua_pushboolean(L, SD.exists(p) || LittleFS.exists(p));
        return 1;
    }

    int lua_sys_millis(lua_State* L)    { lua_pushinteger(L, (lua_Integer)millis()); return 1; }
    int lua_sys_freeHeap(lua_State* L)  { lua_pushinteger(L, (lua_Integer)ESP.getFreeHeap()); return 1; }
    int lua_sys_rng(lua_State* L) {
        lua_pushinteger(L, (lua_Integer)(esp_random() & 0x7FFFFFFF));
        return 1;
    }

    void setFn(lua_State* L, const char* name, lua_CFunction fn) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
    }

    void registerBindings(lua_State* L) {
        lua_register(L, "sleep", lua_sleep);
        lua_register(L, "print", lua_print_bridge);

        lua_newtable(L);
        setFn(L, "setColor", lua_led_setColor);
        setFn(L, "setMode", lua_led_setMode);
        setFn(L, "setBrightness", lua_led_setBrightness);
        lua_setglobal(L, "led");

        lua_newtable(L);
        setFn(L, "clear", lua_gfx_clear);
        setFn(L, "rect", lua_gfx_rect);
        setFn(L, "circle", lua_gfx_circle);
        setFn(L, "text", lua_gfx_text);
        setFn(L, "width", lua_gfx_width);
        setFn(L, "height", lua_gfx_height);
        lua_setglobal(L, "gfx");

        lua_newtable(L);
        setFn(L, "update", lua_input_update);
        setFn(L, "dx", lua_input_dx);
        setFn(L, "click", lua_input_click);
        setFn(L, "quit", lua_input_quit);
        lua_setglobal(L, "input");

        lua_newtable(L);
        setFn(L, "isReady", lua_subghz_isReady);
        setFn(L, "frequency", lua_subghz_frequency);
        setFn(L, "setBand", lua_subghz_setBand);
        setFn(L, "rssi", lua_subghz_rssi);
        setFn(L, "startCapture", lua_subghz_startCapture);
        setFn(L, "isCapturing", lua_subghz_isCapturing);
        setFn(L, "stopCapture", lua_subghz_stopCapture);
        setFn(L, "captureCount", lua_subghz_captureCount);
        setFn(L, "replay", lua_subghz_replay);
        setFn(L, "transmitFile", lua_subghz_transmitFile);
        setFn(L, "transmit", lua_subghz_transmit);
        lua_setglobal(L, "subghz");

        lua_newtable(L);
        setFn(L, "isConnected", lua_wifi_isConnected);
        setFn(L, "ssid", lua_wifi_ssid);
        lua_setglobal(L, "wifi");

        lua_newtable(L);
        setFn(L, "percent", lua_battery_percent);
        setFn(L, "voltage", lua_battery_voltage);
        setFn(L, "charging", lua_battery_charging);
        lua_setglobal(L, "battery");

        lua_newtable(L);
        setFn(L, "isReady", lua_nfc_isReady);
        setFn(L, "uid", lua_nfc_uid);
        setFn(L, "type", lua_nfc_type);
        lua_setglobal(L, "nfc");

        lua_newtable(L);
        setFn(L, "getInt", lua_config_getInt);
        setFn(L, "setInt", lua_config_setInt);
        setFn(L, "getStr", lua_config_getStr);
        setFn(L, "setStr", lua_config_setStr);
        lua_setglobal(L, "config");

        lua_newtable(L);
        setFn(L, "read", lua_fs_read);
        setFn(L, "write", lua_fs_write);
        setFn(L, "exists", lua_fs_exists);
        lua_setglobal(L, "fs");

        lua_newtable(L);
        setFn(L, "millis", lua_sys_millis);
        setFn(L, "freeHeap", lua_sys_freeHeap);
        setFn(L, "rng", lua_sys_rng);
        lua_setglobal(L, "sys");
    }
}

bool LuaEngine::begin() { return true; }

bool LuaEngine::runFile(const String& path) {
    gLastError = "";

    File f = SD.exists(path) ? SD.open(path) : LittleFS.open(path);
    if (!f) { gLastError = "file not found: " + path; return false; }
    String src = f.readString();
    f.close();

    lua_State* L = luaL_newstate();
    if (!L) { gLastError = "luaL_newstate failed (out of memory?)"; return false; }
    // Открываем только нужные стандартные библиотеки. Полный luaL_openlibs
    // тянет package/io/os/debug — лишний расход heap на всё время
    // выполнения скрипта. Задокументированный API прошивки их не использует.
    struct LibReg { const char* name; lua_CFunction fn; };
    static const LibReg libs[] = {
        {"",              luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
    };
    for (const auto& lib : libs) {
        lua_pushcfunction(L, lib.fn);
        lua_pushstring(L, lib.name);
        lua_call(L, 1, 0);
    }
    registerBindings(L);

    bool ok = true;
    if (luaL_loadstring(L, src.c_str()) != 0) {
        gLastError = lua_tostring(L, -1);
        ok = false;
    } else if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        gLastError = lua_tostring(L, -1);
        ok = false;
    }
    lua_close(L);
    return ok;
}

String LuaEngine::lastError() { return gLastError; }
