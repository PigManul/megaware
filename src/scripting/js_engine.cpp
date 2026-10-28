#include "js_engine.h"
#include "../hal/board.h"
#include "../hal/battery.h"
#include "../hal/config.h"
#include "../hal/nfc.h"
#include "../radio/subghz.h"
#include "../net/wifi_manager.h"
#include "game_io.h"
#include <SD.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "../hal/memory_controller.h"

extern "C" {
#include <elk.h>
}

namespace {
    String gLastError;
    // JS runtime is only used when a script runs; keep its fixed arena in PSRAM
    // so idle firmware does not consume internal heap.
    EXT_RAM_ATTR char jsMem[8192];

    jsval_t js_print_fn(struct js* js, jsval_t* args, int nargs) {
        for (int i = 0; i < nargs; i++) { Serial.print(js_str(js, args[i])); Serial.print(' '); }
        Serial.println();
        return js_mkundef();
    }

    jsval_t js_sleep_fn(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkundef();
        long ms = (long)js_getnum(args[0]);
        uint32_t start = millis();
        while ((long)(millis() - start) < ms) {
            Board::updateLeds(millis());
            InputEvent ev = GameIO::poll();
            if (ev == InputEvent::UserKey) return js_mkerr(js, "script aborted (Quit pressed)");
            delay(10);
        }
        return js_mkundef();
    }

    jsval_t js_led_setColor(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 3) return js_mkerr(js, "setColor(r,g,b) needs 3 args");
        Board::setLedColor((uint8_t)js_getnum(args[0]), (uint8_t)js_getnum(args[1]), (uint8_t)js_getnum(args[2]));
        return js_mkundef();
    }
    jsval_t js_led_setMode(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkerr(js, "setMode(n) needs 1 arg");
        Board::setLedMode((LedMode)(int)js_getnum(args[0]));
        return js_mkundef();
    }
    jsval_t js_led_setBrightness(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkerr(js, "setBrightness(n) needs 1 arg");
        Board::setLedBrightness((uint8_t)constrain((int)js_getnum(args[0]), 0, 255));
        return js_mkundef();
    }
    jsval_t js_led_setSpeed(struct js* js, jsval_t* args, int nargs) { if (nargs < 1) return js_mkfalse(); Board::setLedSpeed((uint8_t)constrain((int)js_getnum(args[0]), 10, 255)); return js_mktrue(); }

    jsval_t js_gfx_clear(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 3) return js_mkerr(js, "clear(r,g,b) needs 3 args");
        GameIO::clear((uint8_t)js_getnum(args[0]), (uint8_t)js_getnum(args[1]), (uint8_t)js_getnum(args[2]));
        return js_mkundef();
    }
    jsval_t js_gfx_rect(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 7) return js_mkerr(js, "rect(x,y,w,h,r,g,b) needs 7 args");
        GameIO::rect((int)js_getnum(args[0]), (int)js_getnum(args[1]), (int)js_getnum(args[2]), (int)js_getnum(args[3]),
                     (uint8_t)js_getnum(args[4]), (uint8_t)js_getnum(args[5]), (uint8_t)js_getnum(args[6]));
        return js_mkundef();
    }
    jsval_t js_gfx_circle(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 6) return js_mkerr(js, "circle(x,y,r,r,g,b) needs 6 args");
        GameIO::circle((int)js_getnum(args[0]), (int)js_getnum(args[1]), (int)js_getnum(args[2]),
                       (uint8_t)js_getnum(args[3]), (uint8_t)js_getnum(args[4]), (uint8_t)js_getnum(args[5]));
        return js_mkundef();
    }
    jsval_t js_gfx_text(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 6) return js_mkerr(js, "text(x,y,str,r,g,b) needs 6 args");
        char buf[64];
        size_t len = 0;
        const char* s = js_getstr(js, args[2], &len);
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, s, n);
        buf[n] = 0;
        GameIO::text((int)js_getnum(args[0]), (int)js_getnum(args[1]), buf,
                     (uint8_t)js_getnum(args[3]), (uint8_t)js_getnum(args[4]), (uint8_t)js_getnum(args[5]));
        return js_mkundef();
    }

    jsval_t js_input_update(struct js* js, jsval_t* args, int nargs) { GameIO::poll(); return js_mkundef(); }
    jsval_t js_input_dx(struct js* js, jsval_t* args, int nargs)     { return js_mknum(GameIO::dx()); }
    jsval_t js_input_click(struct js* js, jsval_t* args, int nargs)  { return GameIO::click() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_input_quit(struct js* js, jsval_t* args, int nargs)   { return GameIO::quit() ? js_mktrue() : js_mkfalse(); }

    jsval_t js_subghz_isReady(struct js* js, jsval_t* args, int nargs)   { return SubGhz::isReady() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_subghz_frequency(struct js* js, jsval_t* args, int nargs) { return js_mknum(SubGhz::currentFrequencyMHz()); }
    jsval_t js_subghz_setFrequency(struct js* js, jsval_t* args, int nargs) { if (nargs < 1) return js_mkfalse(); SubGhz::setFrequency((float)js_getnum(args[0])); return js_mktrue(); }
    jsval_t js_subghz_setBand(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkerr(js, "setBand(n) needs 1 arg");
        int b = (int)js_getnum(args[0]);
        SubGhz::setBand(b == 0 ? SubGhzBand::MHz315 : (b == 2 ? SubGhzBand::MHz868 : SubGhzBand::MHz433));
        return js_mkundef();
    }
    jsval_t js_subghz_rssi(struct js* js, jsval_t* args, int nargs) { return js_mknum(SubGhz::readRssiDbm()); }
    jsval_t js_subghz_startCapture(struct js* js, jsval_t* args, int nargs) {
        uint32_t ms = nargs >= 1 ? (uint32_t)js_getnum(args[0]) : 10000;
        SubGhz::startRawCapture(&SubGhz::sharedCapture(), ms);
        return js_mkundef();
    }
    jsval_t js_subghz_isCapturing(struct js* js, jsval_t* args, int nargs) { return SubGhz::isCapturing() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_subghz_stopCapture(struct js* js, jsval_t* args, int nargs) { SubGhz::stopRawCapture(); return js_mkundef(); }
    jsval_t js_subghz_valid(struct js* js, jsval_t* args, int nargs) { return SubGhz::sharedCapture().valid ? js_mktrue() : js_mkfalse(); }
    jsval_t js_subghz_captureCount(struct js* js, jsval_t* args, int nargs) { return js_mknum((double)SubGhz::sharedCapture().count); }
    jsval_t js_subghz_replay(struct js* js, jsval_t* args, int nargs) { return SubGhz::replayRaw(SubGhz::sharedCapture()) ? js_mktrue() : js_mkfalse(); }
    jsval_t js_subghz_start(struct js* js, jsval_t* args, int nargs) { SubGhz::startRawCapture(&SubGhz::sharedCapture(), nargs ? (uint32_t)js_getnum(args[0]) : 10000); return js_mktrue(); }
    jsval_t js_subghz_save(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkfalse(); size_t len = 0; const char* p = js_getstr(js, args[0], &len);
        char path[128]; size_t n = len < sizeof(path)-1 ? len : sizeof(path)-1; memcpy(path,p,n); path[n]=0;
        return SubGhz::saveRaw(path, SubGhz::sharedCapture()) ? js_mktrue() : js_mkfalse();
    }
    jsval_t js_subghz_load(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkfalse(); size_t len = 0; const char* p = js_getstr(js, args[0], &len);
        char path[128]; size_t n = len < sizeof(path)-1 ? len : sizeof(path)-1; memcpy(path,p,n); path[n]=0;
        return SubGhz::loadRaw(path, SubGhz::sharedCapture()) ? js_mktrue() : js_mkfalse();
    }
    jsval_t js_subghz_transmitFile(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkfalse();
        size_t len = 0; const char* p = js_getstr(js, args[0], &len);
        char path[128]; size_t n = len < sizeof(path) - 1 ? len : sizeof(path) - 1; memcpy(path, p, n); path[n] = 0;
        return SubGhz::transmitFile(path) ? js_mktrue() : js_mkfalse();
    }
    jsval_t js_subghz_transmit(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 2) return js_mkfalse();
        uint32_t hz = (uint32_t)js_getnum(args[0]);
        size_t len = 0; const char* p = js_getstr(js, args[1], &len);
        char raw[512]; size_t n = len < sizeof(raw) - 1 ? len : sizeof(raw) - 1; memcpy(raw, p, n); raw[n] = 0;
        return SubGhz::transmitRaw(hz, raw) ? js_mktrue() : js_mkfalse();
    }

    jsval_t js_wifi_isConnected(struct js* js, jsval_t* args, int nargs) { return WifiMgr::isConnected() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_wifi_ssid(struct js* js, jsval_t* args, int nargs) {
        String s = WifiMgr::currentSSID();
        return js_mkstr(js, s.c_str(), s.length());
    }
    jsval_t js_wifi_rssi(struct js* js, jsval_t* args, int nargs) {
        return js_mknum(WifiMgr::isConnected() ? WiFi.RSSI() : 0);
    }
    jsval_t js_wifi_ip(struct js* js, jsval_t* args, int nargs) {
        String s = WifiMgr::isConnected() ? WiFi.localIP().toString() : WifiMgr::apIP();
        return js_mkstr(js, s.c_str(), s.length());
    }
    jsval_t js_wifi_scan(struct js* js, jsval_t* args, int nargs) { WifiMgr::startScan(); return js_mktrue(); }
    jsval_t js_wifi_scanning(struct js* js, jsval_t* args, int nargs) { return WifiMgr::isScanning() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_wifi_count(struct js* js, jsval_t* args, int nargs) { return js_mknum(WifiMgr::resultCount()); }
    jsval_t js_wifi_result(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mknull();
        WifiNetwork n = WifiMgr::result((int)js_getnum(args[0]));
        jsval_t obj = js_mkobj(js);
        js_set(js, obj, "ssid", js_mkstr(js, n.ssid.c_str(), n.ssid.length()));
        js_set(js, obj, "rssi", js_mknum(n.rssi));
        js_set(js, obj, "channel", js_mknum(n.channel));
        js_set(js, obj, "secure", n.secure ? js_mktrue() : js_mkfalse());
        return obj;
    }
    jsval_t js_wifi_connect(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 2) return js_mkerr(js, "wifi.connect(ssid, pass) needs 2 args");
        size_t sl = 0, pl = 0; const char* ss = js_getstr(js, args[0], &sl); const char* pp = js_getstr(js, args[1], &pl);
        char s[96], p[96]; size_t sn = sl < sizeof(s)-1 ? sl : sizeof(s)-1; size_t pn = pl < sizeof(p)-1 ? pl : sizeof(p)-1;
        memcpy(s, ss, sn); s[sn] = 0; memcpy(p, pp, pn); p[pn] = 0;
        WifiMgr::connect(String(s), String(p)); return js_mktrue();
    }
    jsval_t js_fs_exists(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mkfalse();
        size_t len = 0; const char* p = js_getstr(js, args[0], &len);
        char path[128]; size_t n = len < sizeof(path) - 1 ? len : sizeof(path) - 1;
        memcpy(path, p, n); path[n] = 0;
        return (SD.exists(path) || LittleFS.exists(path)) ? js_mktrue() : js_mkfalse();
    }
    jsval_t js_fs_read(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mknull();
        size_t len = 0; const char* p = js_getstr(js, args[0], &len);
        char path[128]; size_t n = len < sizeof(path) - 1 ? len : sizeof(path) - 1;
        memcpy(path, p, n); path[n] = 0;
        File f = SD.exists(path) ? SD.open(path) : LittleFS.open(path);
        if (!f) return js_mknull();
        String text = f.readString(); f.close();
        return js_mkstr(js, text.c_str(), text.length());
    }
    jsval_t js_fs_write(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 2) return js_mkfalse(); size_t pl=0, tl=0; const char* pp=js_getstr(js,args[0],&pl); const char* tt=js_getstr(js,args[1],&tl);
        char path[128]; size_t n=pl<sizeof(path)-1?pl:sizeof(path)-1; memcpy(path,pp,n); path[n]=0;
        File f=SD.open(path,FILE_WRITE); if(!f) f=LittleFS.open(path,FILE_WRITE); if(!f) return js_mkfalse(); f.write((const uint8_t*)tt,tl); f.close(); return js_mktrue();
    }
    jsval_t js_fs_remove(struct js* js, jsval_t* args, int nargs) {
        if(nargs<1) return js_mkfalse(); size_t pl=0; const char* pp=js_getstr(js,args[0],&pl); char path[128]; size_t n=pl<sizeof(path)-1?pl:sizeof(path)-1; memcpy(path,pp,n); path[n]=0;
        return (SD.remove(path) || LittleFS.remove(path)) ? js_mktrue() : js_mkfalse();
    }
    jsval_t js_battery_percent(struct js* js, jsval_t* args, int nargs)  { return js_mknum(Battery::percent()); }
    jsval_t js_battery_voltage(struct js* js, jsval_t* args, int nargs)  { return js_mknum(Battery::voltage()); }
    jsval_t js_battery_charging(struct js* js, jsval_t* args, int nargs) { return Battery::isCharging() ? js_mktrue() : js_mkfalse(); }

    // Копия строки из Elk-арены в NUL-terminated буфер (ключи NVS ≤ 15 симв.)
    void js_copystr(struct js* js, jsval_t v, char* out, size_t cap) {
        size_t len = 0;
        const char* s = js_getstr(js, v, &len);
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, s, n);
        out[n] = 0;
    }

    jsval_t js_nfc_isReady(struct js* js, jsval_t* args, int nargs) { return Nfc::ensure() ? js_mktrue() : js_mkfalse(); }
    jsval_t js_nfc_uid(struct js* js, jsval_t* args, int nargs) {
        Nfc::Tag t;
        if (!Nfc::readTag(t)) return js_mknull();
        char buf[32];
        size_t pos = 0;
        for (uint8_t i = 0; i < t.uidLen && pos + 2 < sizeof(buf); i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", t.uid[i]);
        return js_mkstr(js, buf, pos);
    }
    jsval_t js_nfc_type(struct js* js, jsval_t* args, int nargs) {
        Nfc::Tag t;
        if (!Nfc::readTag(t)) return js_mknull();
        return js_mkstr(js, t.type, strlen(t.type));
    }

    jsval_t js_config_getInt(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mknum(0);
        char key[16];
        js_copystr(js, args[0], key, sizeof(key));
        int def = nargs >= 2 ? (int)js_getnum(args[1]) : 0;
        return js_mknum(Config::getInt(key, def));
    }
    jsval_t js_config_setInt(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 2) return js_mkfalse();
        char key[16];
        js_copystr(js, args[0], key, sizeof(key));
        Config::setInt(key, (int)js_getnum(args[1]));
        return js_mktrue();
    }
    jsval_t js_config_getStr(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 1) return js_mknull();
        char key[16], def[64];
        js_copystr(js, args[0], key, sizeof(key));
        if (nargs >= 2) js_copystr(js, args[1], def, sizeof(def));
        else def[0] = 0;
        String v = Config::getString(key, def);
        return js_mkstr(js, v.c_str(), v.length());
    }
    jsval_t js_config_setStr(struct js* js, jsval_t* args, int nargs) {
        if (nargs < 2) return js_mkfalse();
        char key[16], val[128];
        js_copystr(js, args[0], key, sizeof(key));
        js_copystr(js, args[1], val, sizeof(val));
        Config::setString(key, val);
        return js_mktrue();
    }

    jsval_t js_sys_millis(struct js* js, jsval_t* args, int nargs)   { return js_mknum((double)millis()); }
    jsval_t js_sys_freeHeap(struct js* js, jsval_t* args, int nargs) { return js_mknum((double)ESP.getFreeHeap()); }
    jsval_t js_sys_largestBlock(struct js* js, jsval_t* args, int nargs) { return js_mknum((double)MemoryController::largestBlock()); }
    jsval_t js_sys_freePsram(struct js* js, jsval_t* args, int nargs) { return js_mknum((double)ESP.getFreePsram()); }
    jsval_t js_sys_heapLow(struct js* js, jsval_t* args, int nargs) { return js_mknum((double)MemoryController::minFreeHeap()); }
}

bool JsEngine::begin() { return true; }

bool JsEngine::runFile(const String& path) {
    gLastError = "";

    File f = SD.exists(path) ? SD.open(path) : LittleFS.open(path);
    if (!f) { gLastError = "file not found: " + path; return false; }
    String src = f.readString();
    f.close();

    struct js* js = js_create(jsMem, sizeof(jsMem));
    if (!js) { gLastError = "js_create failed (buffer too small?)"; return false; }

    jsval_t global = js_glob(js);
    js_set(js, global, "print", js_mkfun(js_print_fn));
    js_set(js, global, "sleep", js_mkfun(js_sleep_fn));

    jsval_t led = js_mkobj(js);
    js_set(js, led, "setColor", js_mkfun(js_led_setColor));
    js_set(js, led, "setMode", js_mkfun(js_led_setMode));
    js_set(js, led, "setBrightness", js_mkfun(js_led_setBrightness));
    js_set(js, led, "setSpeed", js_mkfun(js_led_setSpeed));
    js_set(js, global, "led", led);

    jsval_t gfx = js_mkobj(js);
    js_set(js, gfx, "clear", js_mkfun(js_gfx_clear));
    js_set(js, gfx, "rect", js_mkfun(js_gfx_rect));
    js_set(js, gfx, "circle", js_mkfun(js_gfx_circle));
    js_set(js, gfx, "text", js_mkfun(js_gfx_text));
    js_set(js, global, "gfx", gfx);

    jsval_t input = js_mkobj(js);
    js_set(js, input, "update", js_mkfun(js_input_update));
    js_set(js, input, "dx", js_mkfun(js_input_dx));
    js_set(js, input, "click", js_mkfun(js_input_click));
    js_set(js, input, "quit", js_mkfun(js_input_quit));
    js_set(js, global, "input", input);

    jsval_t subghz = js_mkobj(js);
    js_set(js, subghz, "isReady", js_mkfun(js_subghz_isReady));
    js_set(js, subghz, "frequency", js_mkfun(js_subghz_frequency));
    js_set(js, subghz, "setFrequency", js_mkfun(js_subghz_setFrequency));
    js_set(js, subghz, "setBand", js_mkfun(js_subghz_setBand));
    js_set(js, subghz, "rssi", js_mkfun(js_subghz_rssi));
    js_set(js, subghz, "startCapture", js_mkfun(js_subghz_startCapture));
    js_set(js, subghz, "isCapturing", js_mkfun(js_subghz_isCapturing));
    js_set(js, subghz, "stopCapture", js_mkfun(js_subghz_stopCapture));
    js_set(js, subghz, "valid", js_mkfun(js_subghz_valid));
    js_set(js, subghz, "captureCount", js_mkfun(js_subghz_captureCount));
    js_set(js, subghz, "replay", js_mkfun(js_subghz_replay));
    js_set(js, subghz, "start", js_mkfun(js_subghz_start));
    js_set(js, subghz, "save", js_mkfun(js_subghz_save));
    js_set(js, subghz, "load", js_mkfun(js_subghz_load));
    js_set(js, subghz, "transmitFile", js_mkfun(js_subghz_transmitFile));
    js_set(js, subghz, "transmit", js_mkfun(js_subghz_transmit));
    js_set(js, global, "subghz", subghz);

    jsval_t wifi = js_mkobj(js);
    js_set(js, wifi, "isConnected", js_mkfun(js_wifi_isConnected));
    js_set(js, wifi, "ssid", js_mkfun(js_wifi_ssid));
    js_set(js, wifi, "rssi", js_mkfun(js_wifi_rssi));
    js_set(js, wifi, "ip", js_mkfun(js_wifi_ip));
    js_set(js, wifi, "scan", js_mkfun(js_wifi_scan));
    js_set(js, wifi, "scanning", js_mkfun(js_wifi_scanning));
    js_set(js, wifi, "count", js_mkfun(js_wifi_count));
    js_set(js, wifi, "result", js_mkfun(js_wifi_result));
    js_set(js, wifi, "connect", js_mkfun(js_wifi_connect));
    js_set(js, global, "wifi", wifi);

    jsval_t battery = js_mkobj(js);
    js_set(js, battery, "percent", js_mkfun(js_battery_percent));
    js_set(js, battery, "voltage", js_mkfun(js_battery_voltage));
    js_set(js, battery, "charging", js_mkfun(js_battery_charging));
    js_set(js, global, "battery", battery);

    jsval_t nfc = js_mkobj(js);
    js_set(js, nfc, "isReady", js_mkfun(js_nfc_isReady));
    js_set(js, nfc, "uid", js_mkfun(js_nfc_uid));
    js_set(js, nfc, "type", js_mkfun(js_nfc_type));
    js_set(js, global, "nfc", nfc);

    jsval_t config = js_mkobj(js);
    js_set(js, config, "getInt", js_mkfun(js_config_getInt));
    js_set(js, config, "setInt", js_mkfun(js_config_setInt));
    js_set(js, config, "getStr", js_mkfun(js_config_getStr));
    js_set(js, config, "setStr", js_mkfun(js_config_setStr));
    js_set(js, global, "config", config);

    jsval_t sys = js_mkobj(js);
    js_set(js, sys, "millis", js_mkfun(js_sys_millis));
    js_set(js, sys, "freeHeap", js_mkfun(js_sys_freeHeap));
    js_set(js, sys, "largestBlock", js_mkfun(js_sys_largestBlock));
    js_set(js, sys, "freePsram", js_mkfun(js_sys_freePsram));
    js_set(js, sys, "heapLow", js_mkfun(js_sys_heapLow));
    js_set(js, global, "sys", sys);

    jsval_t fs = js_mkobj(js);
    js_set(js, fs, "exists", js_mkfun(js_fs_exists));
    js_set(js, fs, "read", js_mkfun(js_fs_read));
    js_set(js, fs, "write", js_mkfun(js_fs_write));
    js_set(js, fs, "remove", js_mkfun(js_fs_remove));
    js_set(js, global, "fs", fs);

    jsval_t result = js_eval(js, src.c_str(), src.length());
    bool ok = (js_type(result) != JS_ERR);
    if (!ok) gLastError = js_str(js, result);
    return ok;
}

String JsEngine::lastError() { return gLastError; }
