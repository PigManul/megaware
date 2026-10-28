#include "webui.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
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

namespace {
    WebServer* server = nullptr;
    DNSServer* dns = nullptr;
    bool enabled = false; // пользовательский тумблер (Wi-Fi -> WebUI)
    File uploadFile;

    void registerRoutes(); // реализация в конце файла, после всех обработчиков
    void handleTerminal();
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
        item("/ota", SVG_INFO, "UPDATE", "ota");
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
            if (a.startsWith("off")) { SubGhz::webSpectrum(-1); out = "sweep off"; }
            else {
                int band = a.toInt();
                if (band >= 0 && band <= 3) {
                    const static float P[4][3] = {{433.00f, 0.05f, 36}, {868.00f, 0.025f, 25}, {314.00f, 0.05f, 41}, {902.00f, 0.20f, 66}};
                    SubGhz::setSpectrumRange(P[band][0], P[band][1], (int)P[band][2]);
                    SubGhz::webSpectrum(band);
                    out = "sweep on, band " + String(band);
                } else { ok = false; out = "usage: spectrum on <0..3> | spectrum off"; }
            }
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
        } else {
            ok = false;
            out = "unknown cmd (reboot | led r g b | ledmode n | spectrum on 0..3 | off | ping)";
        }
        String j = String("{\"ok\":") + (ok ? "true" : "false") + ",\"out\":\"" + out + "\"}";
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
            "<button class='btn' onclick=\"cmd('reboot')\">Reboot</button></div>"
            "<canvas id='spec' width='640' height='200' style='width:100%;background:#000;border:1px solid #1a733d;margin-top:10px'></canvas>"
            "<form class='row' style='margin-top:14px' onsubmit='return doCmd()'>"
            "<input type='text' id='ci' placeholder='command: led 20 200 80 | ping | ...' style='flex:1'>"
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
            "    var r=await fetch('/api/status');"
            "    var st=await r.json();"
            "    document.getElementById('live').innerHTML="
            "      'IP <b>'+st.ip+'</b> | RSSI <b>'+st.rssi+' dBm</b> | Heap <b>'+Math.round(st.heap/1024)+' KB</b>'"
            "      +' | PSRAM <b>'+Math.round(st.psram/1024)+' KB</b> | Batt <b>'+st.batt+'%</b>'+(st.chg?'(chg)':'')"
            "      +' | Up <b>'+Math.floor(st.uptime/60)+'m</b> | sweep <b>'+(st.sweep?'ON':'off')+'</b>';"
            "    var sr=await fetch('/api/spectrum');"
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
    //  OTA: обновление прошивки файлом .bin прямо из браузера
    // =================================================================
    void handleOtaGet() {
        sendHead("MEGAWARE // Update");
        sendSidebar("ota");
        sendTopbar("firmware update");
        server->sendContent(
            "<div class='pill' style='display:block'>Upload your .bin firmware (PlatformIO: "
            ".pio/build/t-embed-cc1101/firmware.bin). The device verifies the image "
            "and reboots automatically. Current version: " MW_FW_VERSION "</div>"
            "<form method='POST' action='/ota' enctype='multipart/form-data' style='margin-top:16px'>"
            "<input type='file' name='firmware' accept='.bin' required> "
            "<button class='btn' type='submit'>Flash</button></form>");
        sendFoot();
    }

    void handleOtaUpload() {
        HTTPUpload& up = server->upload();
        if (up.status == UPLOAD_FILE_START) {
            Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (up.status == UPLOAD_FILE_WRITE) {
            Update.write(up.buf, up.currentSize);
        } else if (up.status == UPLOAD_FILE_END) {
            Update.end(true);
        }
    }

    void handleOtaPost() {
        if (Update.hasError()) {
            server->send(500, "text/plain", String("OTA failed: ") + Update.errorString() + " (это не firmware.bin?)");
            Update.end();
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
}
