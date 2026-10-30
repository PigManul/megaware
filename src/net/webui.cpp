#include "webui.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <SD.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include "../hal/config.h"
#include "../hal/board.h"
#include "../hal/battery.h"
#include "../hal/memory_controller.h"
#include "../net/wifi_manager.h"
#include "../net/resource_mgr.h"
#include "../radio/subghz.h"
#include "../hal/ir.h"
#include "../hal/nfc.h"
#include "../storage/files.h"
#include "../ui/ui.h"

namespace {
    WebServer* server = nullptr;
    DNSServer* dns = nullptr;
    bool enabled = false; // пользовательский тумблер (Wi-Fi -> WebUI)
    File uploadFile;

    void registerRoutes(); // реализация в конце файла, после всех обработчиков
    void handleTerminal();
    void handleDocs();
    void handleApiStatus();
    void handleApiSpectrum();
    void handleApiCmd();
    void handleOtaGet();
    void handleOtaPost();
    void handleOtaUpload();
    void handleHexEditGet();
    void handleHexSavePost();
    void handleApiApdu();
    void handleTerminalJs();

    void startServer() {
        if (server) return;
        server = new WebServer(80);
        registerRoutes(); // см. внизу файла: все server->on(...)
        server->begin();
    }

    void stopServer() {
        if (!server) return;
        server->stop();
        delete server;
        server = nullptr;
    }

    // DNS привязан к жизни WebUI, а не к AP напрямую: пока :53 клеймен
    // фичей, WebUI не лезет в порт (см. WebUI::loop).
    void stopDns() {
        if (!dns) return;
        dns->stop();
        delete dns;
        dns = nullptr;
    }

    void handleCaptive() {
        server->sendHeader("Location", String("http://") + WifiMgr::apIP() + "/", true);
        server->send(302, "text/plain", "Redirecting to MEGAWARE");
    }

    fs::FS& fsFor(const String& fs) { return fs == "flash" ? (fs::FS&)LittleFS : (fs::FS&)SD; }

    // Ответ /api/cmd собирается вручную: переводы строк и кавычки в out
    // обязаны уходить экранированными, иначе JSON.parse в браузере падает
    String jsonEscape(const String& s) {
        String r;
        r.reserve(s.length() + 8);
        for (unsigned int i = 0; i < s.length(); i++) {
            char c = s[i];
            if (c == '\n') r += "\\n";
            else if (c == '\"') r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else r += c;
        }
        return r;
    }

    bool validFs(const String& fs) { return fs == "sd" || fs == "flash"; }

    bool validPath(const String& path) {
        return path.length() > 0 && path.length() <= 192 && path[0] == '/' &&
               path.indexOf("..") < 0 && path.indexOf('\\') < 0;
    }

    bool validName(const String& name) {
        return name.length() > 0 && name.length() <= 64 && name != "." && name != ".." &&
               name.indexOf('/') < 0 && name.indexOf('\\') < 0;
    }

    bool validateTarget(const String& fs, const String& path) {
        if (validFs(fs) && validPath(path)) return true;
        server->send(400, "text/plain", "invalid filesystem or path");
        return false;
    }

    String htmlEscape(const String& value) {
        String out; out.reserve(value.length() + 16);
        for (size_t i = 0; i < value.length(); ++i) {
            switch (value[i]) {
                case '&': out += F("&amp;"); break;
                case '<': out += F("&lt;"); break;
                case '>': out += F("&gt;"); break;
                case '\"': out += F("&quot;"); break;
                case '\'': out += F("&#39;"); break;
                default: out += value[i]; break;
            }
        }
        return out;
    }

    // --- SVG-иконки (инлайн, без внешних файлов) ---
    const char* SVG_CHIP   = "<svg width='26' height='26' viewBox='0 0 24 24'><rect x='6' y='6' width='12' height='12' rx='1'/><path d='M9 2v3M12 2v3M15 2v3M9 19v3M12 19v3M15 19v3M2 9h3M2 12h3M2 15h3M19 9h3M19 12h3M19 15h3'/></svg>";
    const char* SVG_CHIP_HEX = "<svg width='16' height='16' viewBox='0 0 24 24'><rect x='3' y='5' width='18' height='14' rx='1'/><path d='M7 9h.01M11 9h.01M15 9h.01M7 13h.01M11 13h.01M15 13h.01M7 17h.01M11 17h.01M15 17h.01'/></svg>";
    const char* SVG_FOLDER = "<svg width='18' height='18' viewBox='0 0 24 24'><path d='M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6z'/></svg>";
    const char* SVG_FILE   = "<svg width='18' height='18' viewBox='0 0 24 24'><path d='M6 2h9l5 5v15H6V2z'/><path d='M14 2v6h6'/></svg>";
    const char* SVG_TRASH  = "<svg width='16' height='16' viewBox='0 0 24 24'><path d='M4 7h16M9 7V4h6v3M6 7l1 14h10l1-14'/></svg>";
    const char* SVG_EDIT   = "<svg width='16' height='16' viewBox='0 0 24 24'><path d='M4 20l4-1 11-11-3-3L5 16z'/></svg>";
    const char* SVG_DOWN   = "<svg width='16' height='16' viewBox='0 0 24 24'><path d='M12 3v13m-5-5l5 5 5-5M4 21h16'/></svg>";
    const char* SVG_GEAR   = "<svg width='18' height='18' viewBox='0 0 24 24'><circle cx='12' cy='12' r='3'/><path d='M12 2v3M12 19v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M2 12h3M19 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1'/></svg>";
    const char* SVG_INFO   = "<svg width='18' height='18' viewBox='0 0 24 24'><circle cx='12' cy='12' r='9'/><path d='M12 8h.01M11 12h1v5h1'/></svg>";
    const char* SVG_CHART  = "<svg width='18' height='18' viewBox='0 0 24 24'><path d='M4 19h16M6 17V9m5 8V5m5 12v-6'/></svg>";
    const char* SVG_POWER  = "<svg width='16' height='16' viewBox='0 0 24 24'><path d='M12 2v8M6 6a8 8 0 1 0 12 0'/></svg>";

    String encPath(const String& p) {
        String out; out.reserve(p.length() + 8);
        for (size_t i = 0; i < p.length(); i++) {
            char c = p[i];
            if (isalnum((int)c) || c == '/' || c == '.' || c == '-' || c == '_') out += c;
            else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c); out += buf; }
        }
        return out;
    }

    // =====================================================================
    //  Потоковая отдача страницы: голова + sidebar/topbar + хвост
    //  отправляются кусками через sendContent(), а не собираются в одну
    //  большую String — заметно меньше пиковой памяти на рендер.
    // =====================================================================
    void sendHead(const char* title) {
        server->setContentLength(CONTENT_LENGTH_UNKNOWN);
        server->send(200, "text/html", "");
        server->sendContent(
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>");
        server->sendContent(title);
        server->sendContent(
            "</title><style>"
            "*{box-sizing:border-box}"
            "body{background:#000;color:#b8ffcf;font-family:'Consolas','Courier New',monospace;"
            "margin:0;display:flex;min-height:100vh}"
            "a{color:#39ff88;text-decoration:none}"
            ".side{width:210px;background:#000;border-right:1px solid #1a733d;padding:18px 0;"
            "display:flex;flex-direction:column}"
            ".side .item{display:flex;align-items:center;gap:10px;padding:12px 20px;color:#1a733d;"
            "border-left:3px solid transparent;cursor:pointer}"
            ".side .item.active{background:#03130a;color:#39ff88;border-left-color:#39ff88}"
            ".side .item svg{stroke:currentColor;fill:none;stroke-width:1.7}"
            ".side .foot{margin-top:auto;padding:14px 20px;font-size:12px;color:#124a28;"
            "border-top:1px solid #1a733d;display:flex;align-items:center;gap:8px}"
            ".dot{width:7px;height:7px;border-radius:50%;background:#39ff88}"
            ".main{flex:1;padding:24px 30px;max-width:1000px}"
            ".topbar{display:flex;align-items:center;gap:16px;padding-bottom:14px;border-bottom:1px solid #39ff88;"
            "margin-bottom:20px}"
            ".topbar h1{color:#39ff88;font-size:22px;letter-spacing:1px;margin:0;display:flex;align-items:center;gap:10px}"
            ".topbar .spacer{flex:1}"
            ".topbar .meta{color:#1a733d;font-size:13px;display:flex;align-items:center;gap:14px}"
            ".topbar .pipe{color:#124a28}"
            "form.row{margin:0;display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
            "input[type=file],input[type=text],input[type=password]{background:#000;color:#b8ffcf;"
            "border:1px solid #1a733d;padding:8px;font-family:inherit;font-size:13px}"
            "input[type=submit],.btn,button.btn{background:#000;color:#39ff88;border:1px solid #39ff88;"
            "padding:8px 16px;font-size:13px;cursor:pointer;text-decoration:none;"
            "font-family:inherit;transition:.15s}"
            "input[type=submit]:hover,.btn:hover{background:#39ff88;color:#000}"
            ".toolbar{display:flex;gap:10px;align-items:center;margin-bottom:16px;flex-wrap:wrap}"
            ".pill{border:1px solid #1a733d;padding:8px 16px;display:flex;align-items:center;gap:8px;font-size:13px}"
            ".tabs{margin-left:auto;display:flex;gap:6px}"
            ".tabs a{padding:6px 14px;background:#000;color:#1a733d;border:1px solid #1a733d;font-size:13px}"
            ".tabs a.active{background:#39ff88;color:#000;border-color:#39ff88}"
            "ul{padding:0;margin:0;border:1px solid #1a733d;overflow:hidden}"
            "li{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid #03130a;"
            "background:#000;list-style:none}"
            "li:last-child{border-bottom:none}"
            "li .name{flex:1;color:#b8ffcf;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
            "li .size{color:#124a28;font-size:12px;margin-right:6px;white-space:nowrap}"
            "a.icon,button.icon{background:none;border:none;cursor:pointer;padding:4px;line-height:0}"
            "a.icon svg,button.icon svg{stroke:#1a733d;fill:none;stroke-width:1.7}"
            "a.icon:hover svg,button.icon:hover svg{stroke:#39ff88}"
            "textarea{width:100%;height:60vh;background:#000;color:#b8ffcf;border:1px solid #1a733d;"
            "padding:10px;font-family:inherit;font-size:13px}"
            "label{font-size:12px;color:#1a733d;display:block;margin-bottom:4px}"
            "hr{border:none;border-top:1px solid #1a733d;margin:20px 0}"
            "table{width:100%;border-collapse:collapse}"
            "table td{padding:8px 4px;border-bottom:1px solid #03130a;font-size:13px}"
            "table td.k{color:#1a733d;width:40%}"
            "#prog{display:none;margin-top:14px;padding:14px;border:1px solid #1a733d;"
            "align-items:center;gap:16px}"
            "#progBarWrap{flex:1;height:10px;background:#03130a;overflow:hidden}"
            "#progBar{height:100%;width:0;background:#39ff88;transition:.1s}"
            "</style></head><body>");
    }

    void sendSidebar(const char* active) {
        auto item = [&](const char* href, const char* svg, const char* label, const char* key) {
            String cls = strcmp(active, key) == 0 ? "item active" : "item";
            server->sendContent("<a class='" + cls + "' href='" + href + "'>" + svg + label + "</a>");
        };
        server->sendContent("<div class='side'>");
        item("/", SVG_FOLDER, "FILES", "files");
        item("/system", SVG_CHART, "SYSTEM INFO", "system");
        item("/terminal", SVG_CHART, "TERMINAL", "term");
        item("/about", SVG_INFO, "ABOUT", "about");
        item("/ota", SVG_INFO, "OTA UPDATE", "ota");
        server->sendContent(
            "<div class='foot'><span class='dot'></span> ESP32 Online</div></div><div class='main'>");
    }

    void sendTopbar(const char* subtitle) {
        server->sendContent(String("<div class='topbar'><h1>") + SVG_CHIP + " MEGAWARE</h1>"
            "<div class='spacer'></div><div class='meta'>" + subtitle +
            "<span class='pipe'>|</span>"
            "<form method='POST' action='/shutdown' onsubmit='return confirm(\"Power off the device?\")'>"
            "<button class='btn' type='submit' style='display:flex;align-items:center;gap:6px'>" +
            SVG_POWER + " Shutdown</button></form></div></div>");
    }

    void sendFoot() {
        server->sendContent("</div></body></html>");
        server->sendContent("");
    }

    // =====================================================================
    //  /  — файловый менеджер
    // =====================================================================
    void handleRoot() {
        String fsName = server->hasArg("fs") ? server->arg("fs") : "sd";
        String path = server->hasArg("path") ? server->arg("path") : "/";
        if (!path.length()) path = "/";
        if (!validateTarget(fsName, path)) return;

        sendHead("MEGAWARE // Files");
        sendSidebar("files");

        String spaceInfo = "";
        if (fsName == "sd" && Files::sdPresent()) {
            spaceInfo = String((unsigned long)(SD.totalBytes() / 1024 / 1024)) + " MB total";
        }
        sendTopbar(spaceInfo.c_str());

        server->sendContent("<div class='toolbar'>"
            "<div class='pill'>" + String(SVG_INFO) + " " + WiFi.localIP().toString() + "</div>"
            "<div class='tabs'>"
            "<a class='" + String(fsName == "sd" ? "active" : "") + "' href='/?fs=sd&path=/'>SD card</a>"
            "<a class='" + String(fsName == "flash" ? "active" : "") + "' href='/?fs=flash&path=/'>Flash</a>"
            "</div></div>");

        // хлебные крошки
        {
            String html = "<div style='color:#1a733d;font-size:13px;margin-bottom:10px'><a href='/?fs=" + fsName + "&path=/'>~/root</a>";
            String acc; int start = 1;
            while (start <= (int)path.length()) {
                int slash = path.indexOf('/', start);
                String part = slash < 0 ? path.substring(start) : path.substring(start, slash);
                if (part.length()) {
                    acc += "/" + part;
                    html += " / <a href='/?fs=" + fsName + "&path=" + encPath(acc) + "'>" + part + "</a>";
                }
                if (slash < 0) break;
                start = slash + 1;
            }
            html += "</div>";
            server->sendContent(html);
        }

        fs::FS& fsRef = fsFor(fsName);
        bool mounted = (fsName == "flash") ? Files::flashPresent() : Files::sdPresent();

        server->sendContent("<ul>");
        if (mounted) {
            File dir = fsRef.open(path);
            if (dir && dir.isDirectory()) {
                File f = dir.openNextFile();
                bool any = false;
                while (f) {
                    any = true;
                    // ВАЖНО: полный путь строим от ТЕКУЩЕЙ папки. f.name()
                    // у SD возвращает голое имя — склейка "/"+name теряла
                    // подпапку, из-за чего delete/edit/download молча
                    // работали не с тем файлом (и выкидывали из папки).
                    String base = String(f.name());
                    if (base.startsWith("/")) base = base.substring(base.lastIndexOf('/') + 1);
                    String full = path;
                    if (!full.endsWith("/")) full += "/";
                    full += base;
                    String safeBase = htmlEscape(base);
                    bool isDir = f.isDirectory();

                    String row = "<li>";
                    row += isDir ? SVG_FOLDER : SVG_FILE;
                    if (isDir) {
                        row += "<a class='name' href='/?fs=" + fsName + "&path=" + encPath(full) + "'>" + safeBase + "</a>";
                    } else {
                        row += "<span class='name'>" + safeBase + "</span>";
                        row += "<span class='size'>" + String(f.size()) + " B</span>";
                        row += "<a class='icon' href='/download?fs=" + fsName + "&path=" + encPath(full) + "'>" + SVG_DOWN + "</a>";
                        row += "<a class='icon' href='/edit?fs=" + fsName + "&path=" + encPath(full) + "'>" + SVG_EDIT + "</a>";
                        row += "<a class='icon' href='/hexedit?fs=" + fsName + "&path=" + encPath(full) + "'>" + SVG_CHIP_HEX + "</a>";
                        row += "<form class='row' method='POST' action='/delete' onsubmit='return confirm(\"Delete " + safeBase + "?\")'>"
                               "<input type='hidden' name='fs' value='" + fsName + "'>"
                               "<input type='hidden' name='path' value='" + full + "'>"
                               "<button class='icon' type='submit'>" + SVG_TRASH + "</button></form>";
                    }
                    row += "</li>";
                    server->sendContent(row);
                    f = dir.openNextFile();
                }
                if (!any) server->sendContent("<li>(empty)</li>");
            }
        } else {
            server->sendContent("<li>Not mounted</li>");
        }
        server->sendContent("</ul>");

        // создание файла/папки + загрузка с прогресс-баром (XHR, без перезагрузки страницы).
        // Поддержан drag & drop: кидай файлы прямо на страницу.
        server->sendContent(
            "<div class='toolbar' style='margin-top:16px'>"
            "<form class='row' method='POST' action='/newfile'>"
            "<input type='hidden' name='fs' value='" + fsName + "'><input type='hidden' name='path' value='" + path + "'>"
            "<input type='text' name='name' placeholder='new_file.txt' required>"
            "<input type='submit' value='+ file'></form>"
            "<form class='row' method='POST' action='/newfolder'>"
            "<input type='hidden' name='fs' value='" + fsName + "'><input type='hidden' name='path' value='" + path + "'>"
            "<input type='text' name='name' placeholder='new_folder' required>"
            "<input type='submit' value='+ folder'></form>"
            "<button class='btn' onclick=\"document.getElementById('fileInp').click()\">&#8593; Upload</button>"
            "<a class='btn' href='/hexedit?fs=" + fsName + "&path=" + path + "'>&#x1F5A5; Hex editor</a>"
            "<input type='file' id='fileInp' style='display:none' multiple>"
            "</div>"
            "<div class='pill' style='display:block;margin-top:8px'>drop files anywhere on this page to upload</div>"
            "<div id='prog'><div id='progBarWrap'><div id='progBar'></div></div><span id='progPct'>0%</span></div>"
            "<script>"
            "var upQueue=0;"
            "function upFile(f){"
            "var fd=new FormData(); fd.append('fs','" + fsName + "'); fd.append('path','" + path + "'); fd.append('file',f);"
            "var xhr=new XMLHttpRequest();"
            "document.getElementById('prog').style.display='flex';"
            "xhr.upload.onprogress=function(ev){ if(ev.lengthComputable){"
            "var pct=Math.round(ev.loaded/ev.total*100);"
            "document.getElementById('progBar').style.width=pct+'%';"
            "document.getElementById('progPct').innerText=pct+'%';}};"
            "xhr.onload=function(){ if(--upQueue<=0) location.reload(); };"
            "xhr.onerror=function(){ if(--upQueue<=0) location.reload(); };"
            "xhr.open('POST','/upload'); xhr.send(fd);"
            "}"
            "document.getElementById('fileInp').addEventListener('change',function(e){"
            "upQueue=e.target.files.length; for(var i=0;i<e.target.files.length;i++) upFile(e.target.files[i]); });"
            "['dragover','dragenter'].forEach(function(ev){ document.addEventListener(ev,function(e){ e.preventDefault(); }); });"
            "document.addEventListener('drop',function(e){ e.preventDefault();"
            "var fs=e.dataTransfer.files; upQueue=fs.length; for(var i=0;i<fs.length;i++) upFile(fs[i]); });"
            "</script>"
        );

        sendFoot();
    }

    // =====================================================================
    //  /system — техническая информация (аналог About на устройстве)
    // =====================================================================
    void handleSystem() {
        sendHead("MEGAWARE // System");
        sendSidebar("system");
        sendTopbar("");

        String rows;
        rows += "<tr><td class='k'>Chip</td><td>" + String(ESP.getChipModel()) + " rev" + String(ESP.getChipRevision()) + ", " + String(ESP.getChipCores()) + " cores @ " + String(ESP.getCpuFreqMHz()) + "MHz</td></tr>";
        rows += "<tr><td class='k'>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " / " + String(ESP.getHeapSize() / 1024) + " KB</td></tr>";
        rows += "<tr><td class='k'>Free PSRAM</td><td>" + String(ESP.getFreePsram() / 1024) + " / " + String(ESP.getPsramSize() / 1024) + " KB</td></tr>";
        rows += "<tr><td class='k'>Flash</td><td>" + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</td></tr>";
        rows += "<tr><td class='k'>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
        rows += "<tr><td class='k'>Wi-Fi</td><td>" + (WiFi.status() == WL_CONNECTED ? (WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)") : String("not connected")) + "</td></tr>";
        rows += "<tr><td class='k'>SD card</td><td>" + (Files::sdPresent() ? (String(Files::sdUsedBytes() / 1024 / 1024) + " / " + String(Files::sdTotalBytes() / 1024 / 1024) + " MB") : String("not present")) + "</td></tr>";

        server->sendContent("<table>" + rows + "</table>");
        sendFoot();
    }

    // =====================================================================
    //  /settings — логин/пароль + управление сохранённой Wi-Fi сетью
    // =====================================================================
    void handleForgetWifi() {
        Config::clearKey("wifi_ssid");
        Config::clearKey("wifi_pass");
        server->sendHeader("Location", "/about");
        server->send(303);
    }

    // =====================================================================
    //  /about
    // =====================================================================
    void handleAbout() {
        sendHead("MEGAWARE // About");
        sendSidebar("about");
        sendTopbar("");

        uint32_t up = millis() / 1000;
        auto td = [](const char* k, const String& v) {
            return String("<tr><td class='k'>") + k + "</td><td>" + v + "</td></tr>";
        };
        auto hdr = [](const char* t) {
            return String("<tr><td colspan='2' style='color:#39ff88;padding-top:14px'>") + t + "</td></tr>";
        };

        String h = "<table>";
        h += hdr("CHIP");
        h += td("Model", ESP.getChipModel());
        h += td("Cores", String(ESP.getChipCores()));
        h += td("CPU", String(ESP.getCpuFreqMHz()) + " MHz");
        h += td("Revision", String(ESP.getChipRevision()));
        h += hdr("MEMORY");
        h += td("Heap free", String((unsigned long)(ESP.getFreeHeap()/1024)) + " / " + (unsigned long)(ESP.getHeapSize()/1024) + " KB");
        h += td("PSRAM", String((unsigned long)(ESP.getFreePsram()/1024)) + " / " + (unsigned long)(ESP.getPsramSize()/1024) + " KB");
        h += td("Flash", String((unsigned long)(ESP.getFlashChipSize()/1024/1024)) + " MB");
        h += td("SD", Files::sdPresent() ?
            String((unsigned long)(Files::sdUsedBytes()/1024/1024)) + " / " + (unsigned long)(Files::sdTotalBytes()/1024/1024) + " MB" : "not present");
        h += td("Uptime", String(up/60) + "m " + String(up%60) + "s");
        h += hdr("PINS");
        h += td("Encoder", "A 4 / B 5 / KEY 0");
        h += td("Keys", "Quit 6 / PWR_EN 15");
        h += td("CC1101", "CS 12  GDO0 3  GDO2 38");
        h += td("SD / LED", "CS 13 / DATA 14");
        h += td("I2C", "SDA 8 / SCL 18");
        h += td("IR", "TX 2 / RX 1");
        h += td("Mic / Spk", "42/39  7/40/46");
        h += td("PN532 / NRF", "RST 45 IRQ 17 / CE 43 CS 44");
        h += hdr("MODULES");
        h += td("CC1101", SubGhz::isReady() ? "OK" : "not found");
        h += td("SD card", Files::sdPresent() ? "mounted" : "absent");
        h += td("LittleFS", Files::flashPresent() ? "mounted" : "error");
        h += td("BQ27220", Battery::isPresent() ? "OK" : "not found");
        h += td("BLE", "available (ESP32-S3)");
        h += hdr("FIRMWARE");
        h += td("Version", String(MW_FW_NAME) + " v" + MW_FW_VERSION);
        h += td("Built", String(__DATE__) + " " + __TIME__);
        h += td("Board", "LilyGo T-Embed CC1101");
        h += hdr("NETWORK");
        h += td("Wi-Fi", WifiMgr::isConnected() ? WifiMgr::currentSSID() : "not connected");
        h += td("IP", WifiMgr::isConnected() ? WiFi.localIP().toString() : "—");
        h += td("AP", WifiMgr::isAPRunning() ? "192.168.4.1" : "off");
        h += td("WebUI", "running :80");
        h += "</table>";
        server->sendContent(h);
        sendFoot();
    }

    void handleShutdown() {
        server->send(200, "text/plain", "Powering off...");
        delay(300);
        Board::enterDeepSleep();
    }

    void handleDownload() {
        String fsName = server->arg("fs"), path = server->arg("path");
        if (!validateTarget(fsName, path)) return;
        File f = fsFor(fsName).open(path);
        if (!f) { server->send(404, "text/plain", "not found"); return; }
        server->streamFile(f, "application/octet-stream");
        f.close();
    }

    void handleEditGet() {
        String fsName = server->arg("fs"), p = server->arg("path");
        if (!validateTarget(fsName, p)) return;
        File f = fsFor(fsName).open(p);
        if (!f || f.size() > 16384) { server->send(400, "text/plain", "cannot edit this file"); return; }
        String content = f.readString();
        f.close();

        sendHead("MEGAWARE // Edit");
        sendSidebar("files");
        sendTopbar("");
        server->sendContent("<h2>" + p + "</h2><form method='POST' action='/edit'>"
            "<input type='hidden' name='fs' value='" + fsName + "'><input type='hidden' name='path' value='" + p + "'>"
            "<textarea name='content'>" + htmlEscape(content) + "</textarea><br><br>"
            "<input type='submit' class='btn' value='Save'> "
            "<a class='btn' href='/?fs=" + fsName + "&path=" + p.substring(0, p.lastIndexOf('/') + 1) + "'>Cancel</a></form>");
        sendFoot();
    }

    void handleEditPost() {
        String fsName = server->arg("fs"), p = server->arg("path");
        if (!validateTarget(fsName, p)) return;
        if (server->arg("content").length() > 16384) { server->send(413, "text/plain", "file too large"); return; }
        fsFor(fsName).remove(p);
        File f = fsFor(fsName).open(p, FILE_WRITE);
        if (f) { f.print(server->arg("content")); f.close(); }
        server->sendHeader("Location", "/?fs=" + fsName + "&path=" + p.substring(0, p.lastIndexOf('/') + 1));
        server->send(303);
    }

    void handleDelete() {
        String fsName = server->arg("fs"), p = server->arg("path");
        if (!validateTarget(fsName, p)) return;
        bool ok = fsFor(fsName).remove(p);
        if (!ok) ok = fsFor(fsName).rmdir(p); // папки (пустые)
        if (!ok) { server->send(400, "text/plain", "cannot delete (not empty?)"); return; }
        Files::invalidateCache(); // список на устройстве должен увидеть изменение
        server->sendHeader("Location", "/?fs=" + fsName + "&path=" + p.substring(0, p.lastIndexOf('/') + 1));
        server->send(303);
    }

    void handleNewFile() {
        String fsName = server->arg("fs"), path = server->arg("path"), name = server->arg("name");
        if (!validateTarget(fsName, path) || !validName(name)) { if (!validName(name)) server->send(400, "text/plain", "invalid name"); return; }
        String full = path; if (!full.endsWith("/")) full += "/"; full += name;
        File f = fsFor(fsName).open(full, FILE_WRITE);
        if (f) f.close();
        Files::invalidateCache();
        server->sendHeader("Location", "/?fs=" + fsName + "&path=" + path);
        server->send(303);
    }

    void handleNewFolder() {
        String fsName = server->arg("fs"), path = server->arg("path"), name = server->arg("name");
        if (!validateTarget(fsName, path) || !validName(name)) { if (!validName(name)) server->send(400, "text/plain", "invalid name"); return; }
        String full = path; if (!full.endsWith("/")) full += "/"; full += name;
        fsFor(fsName).mkdir(full);
        Files::invalidateCache();
        server->sendHeader("Location", "/?fs=" + fsName + "&path=" + path);
        server->send(303);
    }

    void handleUploadData() {
        HTTPUpload& upload = server->upload();
        String fsName = server->arg("fs"), base = server->arg("path");
        if (!validFs(fsName) || !validPath(base)) return;
        if (upload.status == UPLOAD_FILE_START) {
            if (!validName(upload.filename)) return;
            String fn = base; if (!fn.endsWith("/")) fn += "/"; fn += upload.filename;
            uploadFile = fsFor(fsName).open(fn, FILE_WRITE);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            if (uploadFile) uploadFile.close();
        }
    }

    void handleUploadDone() {
        Files::invalidateCache();
        server->send(200, "text/plain", "OK");
    }

    void registerRoutes() {
        server->on("/", HTTP_GET, handleRoot);
        server->on("/generate_204", HTTP_GET, handleCaptive);
        server->on("/hotspot-detect.html", HTTP_GET, handleCaptive);
        server->on("/connecttest.txt", HTTP_GET, handleCaptive);
        server->on("/ncsi.txt", HTTP_GET, handleCaptive);
        server->on("/system", HTTP_GET, handleSystem);
        server->on("/terminal", HTTP_GET, handleTerminal);
        server->on("/docs", HTTP_GET, handleDocs);
        server->on("/api/status", HTTP_GET, handleApiStatus);
        server->on("/api/spectrum", HTTP_GET, handleApiSpectrum);
        server->on("/api/cmd", HTTP_POST, handleApiCmd);
        server->on("/ota", HTTP_GET, handleOtaGet);
        server->on("/ota", HTTP_POST, handleOtaPost, handleOtaUpload);
        server->on("/hexedit", HTTP_GET, handleHexEditGet);
        server->on("/hexsave", HTTP_POST, handleHexSavePost);
        server->on("/api/apdu", HTTP_POST, handleApiApdu);
        server->on("/terminal.js", HTTP_GET, handleTerminalJs);

        server->on("/forgetwifi", HTTP_POST, handleForgetWifi);
        server->on("/about", HTTP_GET, handleAbout);
        server->on("/shutdown", HTTP_POST, handleShutdown);
        server->on("/download", HTTP_GET, handleDownload);
        server->on("/edit", HTTP_GET, handleEditGet);
        server->on("/edit", HTTP_POST, handleEditPost);
        server->on("/delete", HTTP_POST, handleDelete);
        server->on("/newfile", HTTP_POST, handleNewFile);
        server->on("/newfolder", HTTP_POST, handleNewFolder);
        server->on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
        server->on("/hexedit", HTTP_GET, handleHexEditGet);
        server->on("/hexsave", HTTP_POST, handleHexSavePost);
        server->on("/api/apdu", HTTP_POST, handleApiApdu);
    }

    // =================================================================
    //  Hex-редактор: бинарные файлы до 16 КБ, постранично по 256 байт
    // =================================================================
    constexpr uint32_t HEXEDIT_MAX = 16384;
    constexpr uint32_t HEX_PAGE = 256;

    void handleHexEditGet() {
        String fsName = server->arg("fs"), p = server->arg("path");
        uint32_t off = (uint32_t)server->arg("off").toInt();
        if (!validateTarget(fsName, p)) return;
        File f = fsFor(fsName).open(p, FILE_READ);
        if (!f) { server->send(400, "text/plain", "cannot open"); return; }
        uint32_t size = f.size();
        if (size > HEXEDIT_MAX) { f.close(); server->send(400, "text/plain", "file > 16 KB"); return; }
        static EXT_RAM_ATTR uint8_t data[HEXEDIT_MAX]; // PSRAM
        memset(data, 0, sizeof(data));
        f.read(data, sizeof(data));
        f.close();

        if (off >= size && size > 0) off = 0;

        sendHead("MEGAWARE // HexEdit");
        sendSidebar("files");
        sendTopbar(p.c_str());

        String html = "<div class='pill' style='display:block'>Size: " + String(size) + " B &nbsp; Offset: " + off + "</div>";

        // навигация
        html += "<div class='toolbar' style='margin-top:10px'>";
        if (off >= 256) html += "<a class='btn' href='/hexedit?fs=" + fsName + "&path=" + p + "&off=" + (off - 256) + "'>◀ Prev</a>";
        if (off + 256 < size) html += "<a class='btn' href='/hexedit?fs=" + fsName + "&path=" + p + "&off=" + (off + 256) + "'>Next ▶</a>";
        html += "<a class='btn' href='/?fs=" + fsName + "&path=" + p.substring(0, p.lastIndexOf('/') + 1) + "'>Back</a></div>";

        // hex-таблица с ASCII-колонкой
        html += "<form method='POST' action='/hexsave'>";
        html += "<input type='hidden' name='fs' value='" + fsName + "'>";
        html += "<input type='hidden' name='path' value='" + p + "'>";
        html += String("<input type='hidden' name='off' value='") + off + "'>";
        html += "<table style='font-family:monospace;font-size:12px;border-collapse:collapse;margin-top:10px'>";
        html += "<tr><td></td>";
        for (int c = 0; c < 16; c++) {
            char hx[8];
            snprintf(hx, sizeof(hx), "%02X", c);
            html += String("<td style='color:#1a733d;text-align:center;padding:2px 4px'>") + hx + "</td>";
        }
        html += "<td style='color:#1a733d;text-align:center;padding:2px 8px'>ASCII</td></tr>";

        uint32_t pageEnd = off + 256;
        if (pageEnd > size) pageEnd = size;
        int rowCount = 0;
        for (uint32_t row = off; row < pageEnd; row += 16) {
            char rowHx[8];
            snprintf(rowHx, sizeof(rowHx), "%04X", (unsigned)row);
            html += String("<tr><td style='color:#39ff88;padding:2px 6px'>") + rowHx + "</td>";
            // hex-ячейки
            for (int i = 0; i < 16; i++) {
                uint32_t a = row + i;
                char nm[12];
                snprintf(nm, sizeof(nm), "b%u", (unsigned)a);
                if (a < pageEnd) {
                    char val[4];
                    snprintf(val, sizeof(val), "%02X", data[a]);
                    html += String("<td style='padding:0'><input name='") + nm + "' value='" + val
                          + "' size='2' maxlength='2' style='width:24px;background:#000;color:#b8ffcf;"
                            "border:1px solid #03130a;font-family:monospace;font-size:12px;text-align:center;padding:1px'></td>";
                } else {
                    html += "<td style='padding:0'></td>";
                }
            }
            // ASCII-декодировка
            html += "<td style='color:#1a733d;padding:2px 8px;white-space:pre'>";
            for (int i = 0; i < 16 && row + i < pageEnd; i++) {
                uint8_t b = data[row + i];
                char asc = (b >= 32 && b < 127) ? (char)b : '.';
                html += String(asc);
            }
            html += "</td></tr>";
            rowCount++;
        }
        html += "</table><br><button class='btn' type='submit'>💾 Save</button></form>";
        html += "<div class='toolbar' style='margin-top:10px'>";
        if (off >= 256) html += "<a class='btn' href='/hexedit?fs=" + fsName + "&path=" + p + "&off=" + (off - 256) + "'>◀ Prev</a>";
        if (off + 256 < size) html += "<a class='btn' href='/hexedit?fs=" + fsName + "&path=" + p + "&off=" + (off + 256) + "'>Next ▶</a>";
        html += "</div>";
        server->sendContent(html);
        sendFoot();
    }

    void handleHexSavePost() {
        String fsName = server->arg("fs"), p = server->arg("path");
        uint32_t off = (uint32_t)server->arg("off").toInt();
        if (!validateTarget(fsName, p)) return;
        File f = fsFor(fsName).open(p, FILE_READ);
        if (!f) { server->send(400, "text/plain", "cannot open"); return; }
        uint32_t size = f.size();
        if (size > HEXEDIT_MAX) { f.close(); server->send(400, "text/plain", "file > 16 KB"); return; }
        static EXT_RAM_ATTR uint8_t data[HEXEDIT_MAX]; // PSRAM
        memset(data, 0, sizeof(data));
        f.read(data, sizeof(data));
        f.close();

        for (uint32_t i = 0; i < HEX_PAGE; i++) {
            String nm = "b" + String((unsigned)(off + i));
            if (!server->hasArg(nm)) continue;
            long v = strtol(server->arg(nm).c_str(), nullptr, 16);
            if (off + i < size && v >= 0 && v <= 255) data[off + i] = (uint8_t)v;
        }
        File w = fsFor(fsName).open(p, FILE_WRITE);
        if (!w) { server->send(500, "text/plain", "cannot write"); return; }
        w.write(data, size);
        w.close();
        Files::invalidateCache();
        server->sendHeader("Location", "/hexedit?fs=" + fsName + "&path=" + p + "&off=" + off);
        server->send(303);
    }

    // =================================================================
    //  APDU-консоль: raw ISO14443-4 команды к метке в поле
    // =================================================================
    void handleApiApdu() {
        String hex = server->arg("apdu");
        hex.trim();
        if (!hex.length()) { server->send(400, "application/json", "{\"err\":\"empty\"}"); return; }
        uint8_t buf[64];
        int n = 0, start = 0;
        while (start < (int)hex.length() && n < (int)sizeof(buf)) {
            while (start < (int)hex.length() && hex[start] == ' ') start++;
            if (start + 1 >= (int)hex.length()) break;
            char two[3] = {hex[start], hex[start + 1], 0};
            buf[n++] = (uint8_t)strtol(two, nullptr, 16);
            start += 2;
        }
        if (!Nfc::busPresent()) { server->send(200, "application/json", "{\"err\":\"PN532 not on bus\"}"); return; }
        if (!Nfc::ensure()) { server->send(200, "application/json", "{\"err\":\"PN532 no response\"}"); return; }
        Nfc::Tag t;
        if (!Nfc::readTag(t)) { server->send(200, "application/json", "{\"err\":\"no tag in field\"}"); return; }
        uint8_t resp[240];
        int respLen = sizeof(resp);
        if (!Nfc::apdu(buf, n, resp, &respLen)) {
            server->send(200, "application/json", "{\"err\":\"inDataExchange failed\"}");
            return;
        }
        String j = "{\"resp\":\"";
        for (int i = 0; i < respLen; i++) {
            char two[4];
            snprintf(two, sizeof(two), "%02X", resp[i]);
            j += two;
        }
        j += "\"}";
        server->send(200, "application/json", j);
    }

    // =================================================================
    //  Terminal: статус real-time + команды + спектрограмма
    // =================================================================
    void handleApiStatus() {
        // ВНИМАНИЕ: каждый элемент — String("...") + значение. Голый
        // "literal" + число — это арифметика УКАЗАТЕЛЯ, а не конкатенация
        // (компилируется и крашит при первом же запросе).
        String j = String("{\"heap\":") + (unsigned long)ESP.getFreeHeap();
        j += String(",\"psram\":") + (unsigned long)ESP.getFreePsram();
        j += String(",\"batt\":") + Battery::percent();
        j += String(",\"chg\":") + (Battery::isCharging() ? "true" : "false");
        j += String(",\"rssi\":") + (WifiMgr::isConnected() ? (int)WiFi.RSSI() : 0);
        j += String(",\"ssid\":\"") + (WifiMgr::isConnected() ? WifiMgr::currentSSID() : "") + "\"";
        j += String(",\"uptime\":") + (unsigned long)(millis() / 1000);
        j += String(",\"ip\":\"") + (WifiMgr::isConnected() ? WiFi.localIP().toString() : WifiMgr::apIP()) + "\"";
        j += String(",\"fw\":\"") + MW_FW_VERSION + "\"";
        j += String(",\"sweep\":") + (SubGhz::webSpectrumActive() ? "true" : "false");
        j += "}";
        // без no-store браузер кэширует GET-ответ: график живёт, а подписи
        // частот (тот же JSON) остаются от старого диапазона
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", j);
    }

    void handleApiSpectrum() {
        SubGhz::SpectrumData& s = SubGhz::sharedSpectrum();
        String j = String("{\"start\":") + String(s.startMHz, 3);
        j += String(",\"step\":") + String(s.stepMHz, 4);
        j += String(",\"count\":") + s.count;
        j += String(",\"age\":") + (unsigned long)(millis() - s.lastUpdateMs);
        j += String(",\"dbm\":[");
        for (int i = 0; i < s.count; i++) {
            if (i) j += ',';
            j += String(s.dbm[i], 1);
        }
        j += "]}";
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", j);
    }

    void handleApiCmd() {
        String cmd = server->arg("cmd");
        String out = "";
        bool ok = true;
        if (cmd == "reboot") {
            out = "rebooting";
            server->send(200, "application/json", "{\"ok\":true,\"out\":\"rebooting\"}");
            delay(400);
            ESP.restart();
            return;
        } else if (cmd.startsWith("led ")) {
            int r = 0, g = 0, b = 0;
            if (sscanf(cmd.c_str() + 4, "%d %d %d", &r, &g, &b) == 3) {
                Board::setLedColor((uint8_t)r, (uint8_t)g, (uint8_t)b);
                Board::setLedMode(LedMode::Solid);
                out = "led set";
            } else { ok = false; out = "usage: led <r> <g> <b>"; }
        } else if (cmd.startsWith("ledmode ")) {
            int m = cmd.substring(8).toInt();
            if (m >= 0 && m <= 4) { Board::setLedMode((LedMode)m); out = "led mode set"; }
            else { ok = false; out = "usage: ledmode 0..4"; }
        } else if (cmd.startsWith("spectrum ")) {
            String a = cmd.substring(9);
            a.trim();
            // "on 2".toInt() молча даёт 0 — все кнопки валились в band 0.
            // Разбираем on/off явно, число берём после "on".
            if (a.startsWith("off")) { SubGhz::webSpectrum(-1); out = "sweep off"; }
            else if (a.startsWith("on")) {
                String rest = a.substring(2);
                rest.trim();
                int band = rest.toInt();
                if (band >= 0 && band <= 3) {
                    const static float P[4][3] = {{433.00f, 0.05f, 36}, {868.00f, 0.025f, 25}, {314.00f, 0.05f, 41}, {902.00f, 0.20f, 66}};
                    SubGhz::setSpectrumRange(P[band][0], P[band][1], (int)P[band][2]);
                    // сброс: без него за новым диапазоном таскались значения dbm
                    // предыдущего (и первая отрисовка могла показать смесь)
                    SubGhz::SpectrumData& sd = SubGhz::sharedSpectrum();
                    for (int i = 0; i < sd.count; i++) sd.dbm[i] = -120;
                    sd.lastUpdateMs = 0;
                    SubGhz::webSpectrum(band);
                    out = "sweep on, band " + String(band) + " (" + String(P[band][0], 2)
                        + ".." + String(P[band][0] + P[band][1] * P[band][2], 2) + " MHz)";
                } else { ok = false; out = "usage: spectrum on <0..3> | spectrum off"; }
            } else { ok = false; out = "usage: spectrum on <0..3> | spectrum off"; }
        } else if (cmd == "ping") {
            out = "pong";
        } else if (cmd.startsWith("ir ")) {
            // ir NEC 0x1FE807F [32]  |  ir PANASONIC 0x4004 0x817C0000
            int sp1 = cmd.indexOf(' ');
            int sp2 = cmd.indexOf(' ', sp1 + 1);
            int sp3 = cmd.indexOf(' ', sp2 + 1);
            if (sp1 < 0) { ok = false; out = "usage: ir <PROTO> <value> [bits]"; }
            else {
                String proto = cmd.substring(sp1 + 1, sp2 > 0 ? sp2 : cmd.length());
                proto.toUpperCase();
                uint64_t value = strtoull(cmd.substring(sp2 + 1).c_str(), nullptr, 0);
                uint16_t bits = sp3 > 0 ? (uint16_t)cmd.substring(sp3 + 1).toInt() : 0;
                ok = Ir::sendValue(proto, value, bits);
                out = ok ? "IR sent" : "unknown proto (NEC SONY RC5 RC6 SAMSUNG LG JVC PANASONIC)";
            }
        } else if (cmd.startsWith("irraw ")) {
            // irraw 38000 9000 -4500 550 -550 ...
            int sp = cmd.indexOf(' ');
            uint32_t hz = (uint32_t)cmd.substring(sp + 1).toInt();
            String body = cmd.substring(cmd.indexOf(' ', sp + 1) + 1);
            uint16_t t[200];
            int n = 0, start = 0;
            while (start < (int)body.length() && n < 200) {
                while (start < (int)body.length() && body[start] == ' ') start++;
                if (start >= (int)body.length()) break;
                int end = body.indexOf(' ', start);
                if (end < 0) end = body.length();
                long v = body.substring(start, end).toInt();
                start = end + 1;
                if (v < 0) v = -v;
                if (v == 0) continue;
                if (v > 65000) v = 65000;
                t[n++] = (uint16_t)v;
            }
            ok = Ir::sendRaw(hz, t, n);
            out = ok ? "IR raw sent" : "bad raw args";
        } else if (cmd == "help") {
            out =
                "help                  this list\n"
                "status                heap, psram, battery, wifi, uptime\n"
                "batt                  battery: voltage, percent, charging\n"
                "free                  heap: free / min / largest / frag\n"
                "psram                 PSRAM: size / free\n"
                "uptime                seconds since boot\n"
                "version               firmware version\n"
                "wifi                  connection status\n"
                "wifi scan             start background scan\n"
                "wifi connect <ssid> [pass]\n"
                "wifi off              turn radio off\n"
                "ls [sd|flash] [dir]   list directory (default: sd /)\n"
                "cat <sd|flash> <file> print file content\n"
                "rm <sd|flash> <file>  delete file\n"
                "bright <10..255>      display backlight\n"
                "theme <0..3>          accent color preset\n"
                "led <r> <g> <b>       encoder LED color (solid)\n"
                "ledmode <0..4>        0 off 1 solid 2 breathing 3 tail 4 rainbow\n"
                "spectrum on <0..3> | off   CC1101 sweep (graph above)\n"
                "ir <PROTO> <value> [bits]\n"
                "irraw <hz> <t1> <t2> ...\n"
                "ping | reboot\n"
                "full docs: /docs";
        } else if (cmd == "status" || cmd == "sysinfo") {
            char l[200];
            snprintf(l, sizeof(l),
                     "fw      %s\n"
                     "heap    %u KB (min %u KB)\n"
                     "psram   %u KB\n"
                     "batt    %d%%%s\n"
                     "wifi    %s\n"
                     "uptime  %lu s",
                     MW_FW_VERSION,
                     (unsigned)(ESP.getFreeHeap() / 1024),
                     (unsigned)(MemoryController::minFreeHeap() / 1024),
                     (unsigned)(ESP.getFreePsram() / 1024),
                     Battery::percent(), Battery::isCharging() ? " (chg)" : "",
                     WifiMgr::isConnected() ? WifiMgr::currentSSID().c_str() : "offline",
                     (unsigned long)(millis() / 1000));
            out = l;
        } else if (cmd == "batt") {
            if (!Battery::isPresent()) out = "no battery (fuel gauge not found)";
            else out = String("voltage ") + String(Battery::voltage(), 3) + " V, "
                     + Battery::percent() + "%" + (Battery::isCharging() ? " (charging)" : "");
        } else if (cmd == "free") {
            out = String("heap free ") + ESP.getFreeHeap()
                + " / min " + MemoryController::minFreeHeap()
                + " / largest " + MemoryController::largestBlock()
                + " / frag " + MemoryController::fragmentationPct() + "%";
        } else if (cmd == "psram") {
            out = String("psram ") + ESP.getPsramSize() + " / free " + ESP.getFreePsram();
        } else if (cmd == "uptime") {
            out = String(millis() / 1000) + " s";
        } else if (cmd == "version") {
            out = MW_FW_VERSION;
        } else if (cmd == "wifi" || cmd.startsWith("wifi ")) {
            String a = cmd == "wifi" ? "" : cmd.substring(5);
            a.trim();
            if (a.length() == 0) {
                if (WifiMgr::isConnected()) {
                    out = String("connected: ") + WifiMgr::currentSSID()
                        + ", ip " + WiFi.localIP().toString()
                        + ", rssi " + WiFi.RSSI() + " dBm";
                } else if (WifiMgr::isAPRunning()) {
                    out = String("AP mode, ip ") + WifiMgr::apIP();
                } else out = "offline";
            } else if (a == "scan") {
                WifiMgr::startScan();
                out = "scan started (results on the Connect page)";
            } else if (a.startsWith("connect")) {
                String rest = a.substring(7);
                rest.trim();
                int sp = rest.indexOf(' ');
                String ssid = sp < 0 ? rest : rest.substring(0, sp);
                String pass = sp < 0 ? "" : rest.substring(sp + 1);
                if (ssid.length()) { WifiMgr::connect(ssid, pass); out = "connecting to " + ssid; }
                else { ok = false; out = "usage: wifi connect <ssid> [pass]"; }
            } else if (a == "off") {
                WifiMgr::disconnect();
                WifiMgr::releaseIdleResources();
                out = "wifi off";
            } else { ok = false; out = "usage: wifi | wifi scan | wifi connect <ssid> [pass] | wifi off"; }
        } else if (cmd.startsWith("ls")) {
            FsRoot root = FsRoot::SD;
            String p = "/";
            String rest = cmd.length() > 2 ? cmd.substring(2) : String("");
            rest.trim();
            if (rest.length()) {
                int sp = rest.indexOf(' ');
                String t0 = sp < 0 ? rest : rest.substring(0, sp);
                String t1 = sp < 0 ? "" : rest.substring(sp + 1);
                t1.trim();
                if (t0 == "flash") { root = FsRoot::Flash; p = t1.length() ? t1 : "/"; }
                else if (t0 == "sd") { p = t1.length() ? t1 : "/"; }
                else p = rest;
            }
            if (!validPath(p)) { ok = false; out = "bad path"; }
            else {
                int n = Files::listCount(root, p);
                if (n < 0) { ok = false; out = "cannot open dir"; }
                else {
                    out = String(p) + " (" + n + " entries)\n";
                    int shown = n < 40 ? n : 40;
                    for (int i = 0; i < shown; i++) {
                        FsEntry e = Files::listEntry(root, p, i);
                        out += e.isDir ? "[D] " : "    ";
                        out += e.name;
                        out += "\n";
                    }
                    if (n > shown) out += "... " + String(n - shown) + " more";
                }
            }
        } else if (cmd.startsWith("cat ")) {
            String rest = cmd.substring(4);
            rest.trim();
            int sp = rest.indexOf(' ');
            String t0 = sp < 0 ? "" : rest.substring(0, sp);
            String t1 = sp < 0 ? "" : rest.substring(sp + 1);
            t1.trim();
            if ((t0 == "sd" || t0 == "flash") && t1.length()) {
                File f = fsFor(t0).open(t1);
                if (!f || f.isDirectory()) { ok = false; out = "cannot open " + t1; }
                else {
                    out = "";
                    int lim = 1500;
                    while (f.available() && lim-- > 0) out += (char)f.read();
                    f.close();
                    if (lim <= 0) out += "\n... (truncated)";
                }
            } else { ok = false; out = "usage: cat <sd|flash> <file>"; }
        } else if (cmd.startsWith("rm ")) {
            String rest = cmd.substring(3);
            rest.trim();
            int sp = rest.indexOf(' ');
            String t0 = sp < 0 ? "" : rest.substring(0, sp);
            String t1 = sp < 0 ? "" : rest.substring(sp + 1);
            t1.trim();
            if ((t0 == "sd" || t0 == "flash") && validPath(t1)) {
                bool done = fsFor(t0).remove(t1);
                Files::invalidateCache();
                ok = done;
                out = done ? "removed " + t1 : "remove failed";
            } else { ok = false; out = "usage: rm <sd|flash> <file>"; }
        } else if (cmd.startsWith("bright ")) {
            int v = cmd.substring(7).toInt();
            if (v >= 10 && v <= 255) { ui.webSetBrightness((uint8_t)v); out = "brightness " + String(v); }
            else { ok = false; out = "usage: bright <10..255>"; }
        } else if (cmd.startsWith("theme ")) {
            int v = cmd.substring(6).toInt();
            if (ui.webSetAccentPreset(v)) out = "theme preset " + String(v);
            else { ok = false; out = "usage: theme <0..3>"; }
        } else {
            ok = false;
            out = "unknown cmd - type 'help' (full docs: /docs)";
        }
        String j = String("{\"ok\":") + (ok ? "true" : "false") + ",\"out\":\"" + jsonEscape(out) + "\"}";
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", j);
    }

    void handleTerminal() {
        sendHead("MEGAWARE // Terminal");
        sendSidebar("term");
        sendTopbar("console / spectrum");
        server->sendContent(
            "<div id='live' class='pill' style='display:block'>...</div>"
            "<div class='toolbar' style='margin-top:14px'>"
            "<button class='btn' onclick=\"cmd('spectrum on 0')\">Sweep 433</button>"
            "<button class='btn' onclick=\"cmd('spectrum on 1')\">Sweep 868</button>"
            "<button class='btn' onclick=\"cmd('spectrum on 2')\">Sweep 315</button>"
            "<button class='btn' onclick=\"cmd('spectrum on 3')\">Sweep 915</button>"
            "<button class='btn' onclick=\"cmd('spectrum off')\">Sweep OFF</button>"
            "<button class='btn' onclick=\"cmd('reboot')\">Reboot</button>"
            "<button class='btn' onclick=\"location.href='/docs'\">Command reference</button></div>"
            "<canvas id='spec' width='640' height='200' style='width:100%;background:#000;border:1px solid #1a733d;margin-top:10px'></canvas>"
            "<form class='row' style='margin-top:14px' onsubmit='return doCmd()'>"
            "<input type='text' id='ci' placeholder=\"type 'help' for the command list\" style='flex:1'>"
            "<button class='btn' type='submit'>Send</button></form>"
            "<h3 style='color:#39ff88'>PN532 APDU</h3>"
            "<form class='row' onsubmit='return doApdu()'>"
            "<input type='text' id='apdu-in' placeholder='hex APDU, e.g. FF CA 00 00 00 (UID)' style='flex:1'>"
            "<button class='btn' type='submit'>Send APDU</button></form>"
            "<pre id='log' style='color:#b8ffcf;font-size:12px;max-height:200px;overflow:auto'></pre>"
            "<script src='/terminal.js'></script>");
        sendFoot();
    }

    // =================================================================
    //  Terminal JS: отдельный эндпоинт (raw literal ломал JS)
    // =================================================================
    void handleTerminalJs() {
        server->send(200, "text/javascript",
            "async function cmd(c){"
            "  try{"
            "    var r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)});"
            "    var j=await r.json();"
            "    var lg=document.getElementById('log');"
            "    lg.textContent='> '+c+'\\n'+(j.ok?'':'ERR: ')+j.out+'\\n'+lg.textContent;"
            "  }catch(e){alert('cmd error: '+e);}"
            "}"
            "async function doCmd(){"
            "  var c=document.getElementById('ci').value.trim();"
            "  if(!c)return false;"
            "  await cmd(c);"
            "  document.getElementById('ci').value='';"
            "  return false;"
            "}"
            "async function doApdu(){"
            "  var c=document.getElementById('apdu-in').value.trim();"
            "  if(!c)return false;"
            "  try{"
            "    var r=await fetch('/api/apdu',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'apdu='+encodeURIComponent(c)});"
            "    var j=await r.json();"
            "    var lg=document.getElementById('log');"
            "    lg.textContent='> APDU '+c+'\\n'+(j.err?'ERR: '+j.err:'resp: '+j.resp)+'\\n'+lg.textContent;"
            "  }catch(e){alert('apdu error: '+e);}"
            "  return false;"
            "}"
            "async function poll(){"
            "  try{"
                    "    var r=await fetch('/api/status?t='+Date.now());"
            "    var st=await r.json();"
            "    document.getElementById('live').innerHTML="
            "      'IP <b>'+st.ip+'</b> | RSSI <b>'+st.rssi+' dBm</b> | Heap <b>'+Math.round(st.heap/1024)+' KB</b>'"
            "      +' | PSRAM <b>'+Math.round(st.psram/1024)+' KB</b> | Batt <b>'+st.batt+'%</b>'+(st.chg?'(chg)':'')"
            "      +' | Up <b>'+Math.floor(st.uptime/60)+'m</b> | sweep <b>'+(st.sweep?'ON':'off')+'</b>';"
            "    var sr=await fetch('/api/spectrum?t='+Date.now());"
            "    var sp=await sr.json();"
            "    var cv=document.getElementById('spec');"
            "    var ctx=cv.getContext('2d');"
            "    ctx.fillStyle='#000';ctx.fillRect(0,0,cv.width,cv.height);"
            "    ctx.strokeStyle='#1a733d';ctx.strokeRect(0.5,0.5,cv.width-1,cv.height-1);"
            "    if(sp.count>0&&sp.age<5000){"
            "      var bw=cv.width/sp.count;"
            "      ctx.fillStyle='#39ff88';"
            "      var peak=-999,pi=0;"
            "      for(var i=0;i<sp.count;i++){"
            "        var v=Math.max(0,Math.min(1,(sp.dbm[i]+100)/60.0));"
            "        var h=v*(cv.height-24);"
            "        ctx.fillRect(i*bw,cv.height-12-h,Math.max(1,bw-1),h);"
            "        if(sp.dbm[i]>peak){peak=sp.dbm[i];pi=i;}"
            "      }"
            "      ctx.fillStyle='#b8ffcf';ctx.font='11px monospace';"
            "      ctx.fillText(sp.start.toFixed(2)+' MHz',6,12);"
            "      ctx.fillText((sp.start+sp.step*sp.count).toFixed(2)+' MHz',cv.width-90,12);"
            "      ctx.fillText('peak '+peak.toFixed(0)+' dBm @ '+(sp.start+sp.step*pi).toFixed(2),cv.width/2-60,12);"
            "    }else{"
            "      ctx.fillStyle='#1a733d';ctx.font='12px monospace';"
            "      ctx.fillText('sweep off - press Sweep button or open Spectrum on device',20,100);"
            "    }"
            "  }catch(e){"
            "    document.getElementById('live').innerHTML='<span style=color:#ff5050>ERR: '+e+'</span>';"
            "  }"
            "}"
            "setInterval(poll,600);poll();");
    }

    // =================================================================
    //  Docs: полный справочник команд терминала (English)
    // =================================================================
    void handleDocs() {
        sendHead("MEGAWARE // Docs");
        sendSidebar("term");
        sendTopbar("terminal command reference");
        server->sendContent(
            "<div class='pill' style='display:block'>Open the <a href='/terminal' style='color:#39ff88'>Terminal</a> "
            "and type a command below. Every command replies with a JSON status; the terminal prints "
            "the <b>out</b> field. Type <b>help</b> anytime for the short list.</div>"
            "<h3 style='color:#39ff88'>System</h3>"
            "<table>"
            "<tr><td><b>help</b></td><td>Print the full command list.</td></tr>"
            "<tr><td><b>status</b> / <b>sysinfo</b></td><td>Firmware version, heap (current/minimum), PSRAM, battery percent, Wi-Fi state, uptime.</td></tr>"
            "<tr><td><b>free</b></td><td>Heap details: free, minimum watermark, largest free block, fragmentation %.</td></tr>"
            "<tr><td><b>psram</b></td><td>PSRAM size and free bytes.</td></tr>"
            "<tr><td><b>batt</b></td><td>Battery voltage, percent and charging flag (BQ27220 fuel gauge).</td></tr>"
            "<tr><td><b>uptime</b></td><td>Seconds since boot.</td></tr>"
            "<tr><td><b>version</b></td><td>Firmware version string.</td></tr>"
            "<tr><td><b>ping</b></td><td>Health check, replies <i>pong</i>.</td></tr>"
            "<tr><td><b>reboot</b></td><td>Restart the device immediately.</td></tr>"
            "</table>"
            "<h3 style='color:#39ff88'>Files (sd | flash)</h3>"
            "<div class='pill' style='display:block'>Most file commands accept an optional filesystem prefix: "
            "<b>sd</b> (SD card) or <b>flash</b> (internal LittleFS). Default is <b>sd</b>.</div>"
            "<table>"
            "<tr><td><b>ls</b> [sd|flash] [dir]</td><td>List a directory. Examples: <i>ls</i>, <i>ls flash /</i>, <i>ls /subghz</i>. Dirs are marked [D], 40 entries max per reply.</td></tr>"
            "<tr><td><b>cat</b> &lt;sd|flash&gt; &lt;file&gt;</td><td>Print a text file (first 1500 bytes). Example: <i>cat flash /rfid/keys.txt</i></td></tr>"
            "<tr><td><b>rm</b> &lt;sd|flash&gt; &lt;file&gt;</td><td>Delete a file. This cannot be undone. Example: <i>rm sd /ir/tv.sub</i></td></tr>"
            "</table>"
            "<h3 style='color:#39ff88'>Wi-Fi</h3>"
            "<table>"
            "<tr><td><b>wifi</b></td><td>Connection status: SSID, IP, RSSI — or AP mode info.</td></tr>"
            "<tr><td><b>wifi scan</b></td><td>Start a background scan; results appear on the Connect page of the WebUI.</td></tr>"
            "<tr><td><b>wifi connect</b> &lt;ssid&gt; [pass]</td><td>Join a network. On success the credentials are saved and used for auto-connect at boot. Example: <i>wifi connect Home s3cretpass</i></td></tr>"
            "<tr><td><b>wifi off</b></td><td>Disconnect and power the radio down (frees RAM).</td></tr>"
            "</table>"
            "<h3 style='color:#39ff88'>Display & LED</h3>"
            "<table>"
            "<tr><td><b>bright</b> &lt;10..255&gt;</td><td>Set display backlight now and persist it to NVS.</td></tr>"
            "<tr><td><b>theme</b> &lt;0..3&gt;</td><td>Accent color preset: 0 Green, 1 Amber, 2 Cyan, 3 White. Persisted.</td></tr>"
            "<tr><td><b>led</b> &lt;r&gt; &lt;g&gt; &lt;b&gt;</td><td>Encoder LED color, switches mode to Solid. Example: <i>led 20 200 80</i></td></tr>"
            "<tr><td><b>ledmode</b> &lt;0..4&gt;</td><td>0 Off, 1 Solid, 2 Breathing, 3 Tail, 4 Rainbow.</td></tr>"
            "</table>"
            "<div class='pill' style='display:block'>On-device LED customization (including <b>Click blip</b> and "
            "<b>Rotate glow</b> encoder feedback) lives in <i>Settings → Customize → LED</i> on the device itself.</div>"
            "<h3 style='color:#39ff88'>SubGHz sweep</h3>"
            "<table>"
            "<tr><td><b>spectrum on</b> &lt;0..3&gt;</td><td>Start CC1101 RF sweep for the graph above. Bands: 0 = 433 MHz, 1 = 868 MHz, 2 = 315 MHz, 3 = 915 MHz.</td></tr>"
            "<tr><td><b>spectrum off</b></td><td>Stop the sweep and release the radio.</td></tr>"
            "</table>"
            "<div class='pill' style='display:block'>The sweep only runs while no device feature owns the CC1101. "
            "The buttons above send exactly these commands.</div>"
            "<h3 style='color:#39ff88'>IR</h3>"
            "<table>"
            "<tr><td><b>ir</b> &lt;PROTO&gt; &lt;value&gt; [bits]</td><td>Send an infrared code. Protocols: NEC, SONY, RC5, RC6, SAMSUNG, LG, JVC, PANASONIC. "
            "Value accepts decimal or 0x-hex. Example: <i>ir NEC 0x1FE807F</i></td></tr>"
            "<tr><td><b>irraw</b> &lt;hz&gt; &lt;t1&gt; &lt;t2&gt; ...</td><td>Send raw IR timings in microseconds (sign is ignored). Example: <i>irraw 38000 9000 -4500 550 -550</i></td></tr>"
            "</table>"
            "<h3 style='color:#39ff88'>Flashing the firmware</h3>"
            "<table>"
            "<tr><td><b>OTA over Wi-Fi</b></td><td>The <i>OTA UPDATE</i> page on this site (button there says "
            "\"Flash over OTA\"). Uploads <i>firmware.bin</i> from the browser into the second app slot — "
            "the file must fit 4.5 MB. USB cable not involved.</td></tr>"
            "<tr><td><b>USB cable</b></td><td><code>pio run -t upload</code> from the project folder "
            "(PlatformIO). Full reflash including the partition table — any size. "
            "See <i>docs/FLASHING.md</i> in the repository.</td></tr>"
            "<tr><td><b>On-device from file</b></td><td>Put <i>firmware.bin</i> on the SD card or flash storage "
            "(drag &amp; drop on the FILES page), then on the device open <i>Files</i>, navigate to the file and "
            "<b>hold OK</b> on it. The device verifies the size, flashes the second slot and reboots.</td></tr>"
            "</table>"
            "<div class='pill' style='display:block'>Do not confuse the three: OTA = browser over Wi-Fi, "
            "USB = PlatformIO over cable, on-device = hold OK on a .bin in Files.</div>");
        sendFoot();
    }

    // =================================================================
    //  OTA: обновление прошивки файлом .bin прямо из браузера
    // =================================================================
    void handleOtaGet() {
        sendHead("MEGAWARE // OTA update");
        sendSidebar("ota");
        sendTopbar("ota firmware update");
        server->sendContent(
            "<div class='pill' style='display:block'>This page is <b>OTA over Wi-Fi</b>: the .bin is uploaded "
            "from your browser into the second app slot (max 4.5 MB). <b>It is not USB flashing.</b> "
            "The device verifies the image and reboots automatically. Current version: " MW_FW_VERSION "</div>"
            "<div class='pill' style='display:block;margin-top:8px'>Three ways to flash firmware:</div>"
            "<table style='margin-top:8px'>"
            "<tr><td><b>OTA</b> (this page)</td><td>Upload <i>firmware.bin</i> from the browser over Wi-Fi. "
            "File must fit the OTA slot (4.5 MB) — use <i>.pio/build/t-embed-cc1101/firmware.bin</i>.</td></tr>"
            "<tr><td><b>USB cable</b></td><td>PlatformIO: <code>pio run -t upload</code>. Full reflash, "
            "any size — see <i>docs/FLASHING.md</i>.</td></tr>"
            "<tr><td><b>On-device</b></td><td>Copy <i>firmware.bin</i> to SD or flash storage, open "
            "<i>Files</i> on the device, hold OK on the file.</td></tr>"
            "</table>"
            "<form method='POST' action='/ota' enctype='multipart/form-data' style='margin-top:16px'>"
            "<input type='file' name='firmware' accept='.bin' required> "
            "<button class='btn' type='submit'>Flash over OTA</button></form>");
        sendFoot();
    }

    char otaErr[160] = "";

    void handleOtaUpload() {
        HTTPUpload& up = server->upload();
        if (up.status == UPLOAD_FILE_START) {
            otaErr[0] = 0;
            const esp_partition_t* running = esp_ota_get_running_partition();
            const esp_partition_t* next = esp_ota_get_next_update_partition(running);
            if (!next) {
                // на чипе нет app0/app1 (старая однораздельная таблица)
                snprintf(otaErr, sizeof(otaErr),
                         "No OTA partition on chip. Reflash via USB cable once — then OTA works.");
                return;
            }
            if (!Update.begin(next->size, U_FLASH)) {
                snprintf(otaErr, sizeof(otaErr), "Begin failed: %s", Update.errorString());
            }
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (Update.isRunning() && !otaErr[0]) {
                size_t w = Update.write(up.buf, up.currentSize);
                if (w != up.currentSize && !otaErr[0]) {
                    snprintf(otaErr, sizeof(otaErr), "Write failed: %s", Update.errorString());
                }
            }
        } else if (up.status == UPLOAD_FILE_END) {
            if (Update.isRunning() && !Update.end(true) && !otaErr[0]) {
                snprintf(otaErr, sizeof(otaErr), "Verify failed: %s", Update.errorString());
            }
        } else if (up.status == UPLOAD_FILE_ABORTED) {
            if (Update.isRunning()) Update.end();
            snprintf(otaErr, sizeof(otaErr), "Upload aborted.");
        }
    }

    void handleOtaPost() {
        if (otaErr[0] || Update.hasError()) {
            String msg = otaErr[0] ? String(otaErr) : String("OTA failed: ") + Update.errorString();
            server->send(500, "text/plain", msg);
            otaErr[0] = 0;
            if (Update.isRunning()) Update.end();
            return;
        }
        server->send(200, "text/plain", "OK — firmware accepted, rebooting...");
        delay(800);
        ESP.restart();
    }
} // namespace

void WebUI::start() {
    enabled = true;
    // Если :80 клеймен фичей — не влезаем: сервер поднимется лениво в
    // loop() после release. WebUI не хозяин порта.
    if (!ResMgr::isHeld(ResMgr::Resource::Http80)) startServer();
}

void WebUI::stop() {
    enabled = false;
    stopServer();
    stopDns();
}

bool WebUI::isRunning() { return server != nullptr; }

void WebUI::loop() {
    if (!enabled) return; // выключен — честный no-op, ноль работы в тике

    // Арбитраж с фичами: клейм на :80/:53 выгоняет нас с ресурса,
    // release возвращает. Состояние enabled при этом сохраняется.
    if (ResMgr::isHeld(ResMgr::Resource::Http80)) stopServer();
    else startServer(); // no-op, если уже поднят; отложенный старт после release

    if (ResMgr::isHeld(ResMgr::Resource::Dns53)) stopDns();
    else if (!dns && WifiMgr::isAPRunning()) {
        // DNS лениво: если AP включили ПОСЛЕ старта WebUI — поднимаем тут.
        dns = new DNSServer();
        dns->start(53, "*", WiFi.softAPIP());
    }

    if (dns) dns->processNextRequest();
    if (server) server->handleClient();

    // Веб-sweep: пока веб просит диапазон и на устройстве не запущена своя
    // SubGhz-фича — сами крутим CC1101. Раньше никто не вызывал sweepOnce(),
    // /api/spectrum вечно отдавал устаревшие данные и график не «жил».
    if (SubGhz::webSpectrumActive() && !ui.featureActive()) {
        static uint32_t lastSweep = 0;
        uint32_t now = millis();
        if (now - lastSweep >= 60) { lastSweep = now; SubGhz::sweepOnce(); }
    }
}
