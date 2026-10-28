// =====================================================================
//  Wi-Fi: HTTP — клиент вебхуков/API по пресетам. Пресеты — файлы
//  /http/*.req, создаются через WebUI, формат:
//      POST https://example.com/hook
//      Content-Type: text/plain
//
//      тело запроса
//  Первая строка "МЕТОД URL", дальше заголовки до пустой строки,
//  остаток — тело. Клик — отправить; код и кусок ответа на экране.
//  HTTPS — без проверки сертификата (setInsecure).
// =====================================================================
#include "../ui/features.h"
#include "../net/wifi_manager.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum State : uint8_t { List, Result };
    State state = List;
    int selIndex = 0;
    char resultCode[16] = "";
    char resultSnippet[168] = "";

    fs::FS& httpFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    // заголовки из пресета
    constexpr int MAX_HEADERS = 6;
    String headerKeys[MAX_HEADERS];
    String headerVals[MAX_HEADERS];
    int headerCount = 0;
    void pendingHeaders_add(const String& k, const String& v) {
        if (headerCount < MAX_HEADERS) {
            headerKeys[headerCount] = k;
            headerVals[headerCount] = v;
            headerCount++;
        }
    }

    void runRequest(const char* name) {
        snprintf(resultCode, sizeof(resultCode), "ERR");
        snprintf(resultSnippet, sizeof(resultSnippet), "");
        headerCount = 0;

        if (!WifiMgr::isConnected()) {
            snprintf(resultSnippet, sizeof(resultSnippet), "connect to Wi-Fi first");
            state = Result;
            return;
        }

        String path = String("/http/") + name;
        File f = httpFs().open(path, FILE_READ);
        if (!f) { snprintf(resultSnippet, sizeof(resultSnippet), "open failed"); state = Result; return; }
        String text = f.readString();
        f.close();

        int lineEnd = text.indexOf('\n');
        String first = lineEnd < 0 ? text : text.substring(0, lineEnd);
        first.trim();
        int sp = first.indexOf(' ');
        String method = sp > 0 ? first.substring(0, sp) : "GET";
        String url = sp > 0 ? first.substring(sp + 1) : "";
        method.toUpperCase();
        if (url.length() == 0) { snprintf(resultSnippet, sizeof(resultSnippet), "bad preset: no URL"); state = Result; return; }

        // заголовки до пустой строки
        int pos = lineEnd < 0 ? text.length() : lineEnd + 1;
        while (pos < (int)text.length()) {
            int nl = text.indexOf('\n', pos);
            String line = nl < 0 ? text.substring(pos) : text.substring(pos, nl);
            line.trim();
            pos = nl < 0 ? text.length() : nl + 1;
            if (line.length() == 0) break;
            int colon = line.indexOf(':');
            if (colon > 0) pendingHeaders_add(line.substring(0, colon), line.substring(colon + 1));
        }
        String body = text.substring(pos);
        body.trim();

        HTTPClient http;
        WiFiClientSecure secure;
        WiFiClient plain;
        if (url.startsWith("https://")) {
            secure.setInsecure(); // тестовый клиент — без проверки цепочки
            http.begin(secure, url);
        } else {
            http.begin(plain, url);
        }
        for (int i = 0; i < headerCount; i++) http.addHeader(headerKeys[i], headerVals[i]);

        int code;
        if (method == "POST") code = http.POST(body);
        else if (method == "PUT") code = http.PUT(body);
        else code = http.GET();

        snprintf(resultCode, sizeof(resultCode), "HTTP %d", code);
        if (code > 0) {
            // читаем только первый килобайт ответа: getString() тянет всё
            // тело в heap (пара мегабайт HTML = гарантированный overflow)
            WiFiClient* st = http.getStreamPtr();
            char buf[221];
            int got = st ? st->readBytes(buf, sizeof(buf) - 1) : 0;
            if (got < 0) got = 0;
            buf[got] = 0;
            snprintf(resultSnippet, sizeof(resultSnippet), "%s", buf);
        } else {
            snprintf(resultSnippet, sizeof(resultSnippet), "%s", http.errorToString(code).c_str());
        }
        http.end();
        state = Result;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "HTTP CLIENT");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);

        if (state == Result) {
            uint16_t col = resultCode[5] == '2' ? Theme::OK : Theme::ERR; // "HTTP 2xx"
            c.setTextColor(col, Theme::BG);
            c.drawString(resultCode, x0 + 12, y0 + 8);
            // кусок ответа по словам-строкам
            int row = 0;
            int rows = (h - 34) / Theme::ROW_LIST;
            int start = 0;
            while (start < (int)strlen(resultSnippet) && row < rows) {
                int len = strlen(resultSnippet) - start;
                if (len > 46) len = 46;
                char buf[48];
                memcpy(buf, resultSnippet + start, len);
                buf[len] = 0;
                c.setTextColor(Theme::TEXT_DIM, Theme::BG);
                c.drawString(buf, x0 + 12, y0 + 30 + row * Theme::ROW_LIST);
                start += len;
                row++;
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: back to presets", x0 + 12, y0 + h - 4);
            return;
        }

        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        int n = Files::listCount(root, "/http");
        int visible = (h - 10) / Theme::ROW_LIST;
        int selC = n ? (selIndex % n + n) % n : 0;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < visible && top + i < n; i++) {
            int idx = top + i;
            FsEntry e = Files::listEntry(root, "/http", idx);
            bool s = (idx == selC);
            int y = y0 + 6 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) {
                c.setTextColor(Theme::ACCENT, Theme::BG);
                c.drawString(">", x0, y);
            }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(e.name, x0 + 12, y);
        }
        if (n == 0) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("no presets", x0 + 12, y0 + 6);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("upload /http/*.req via WebUI:", x0 + 12, y0 + 24);
            c.drawString("line1: GET https://...", x0 + 12, y0 + 40);
        } else {
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: send request", x0 + 12, y0 + h - 4);
        }
    }

    bool input(const InputState& in) {
        FsRoot root = Files::sdPresent() ? FsRoot::SD : FsRoot::Flash;
        if (state == Result) {
            if (in.event == InputEvent::Click) { state = List; Features::requestRedraw(); return true; }
            return false;
        }
        int n = Files::listCount(root, "/http");
        if (in.rotationSteps != 0 && n > 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click && n > 0) {
            int selC = (selIndex % n + n) % n;
            FsEntry e = Files::listEntry(root, "/http", selC);
            runRequest(e.name);
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("HTTP", MwCat::Wifi, MwIcon::Terminal, 30, nullptr, input, tick, draw, nullptr);
