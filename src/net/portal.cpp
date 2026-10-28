#include "portal.h"
#include "resource_mgr.h"
#include "wifi_manager.h"
#include "webui.h"
#include "storage/files.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    WebServer* server = nullptr;
    DNSServer* dns = nullptr;
    bool running = false;
    uint32_t hitCount = 0;
    char pagePath[80] = "";      // "" = встроенная страница
    char pageNameStr[40] = "(built-in)";

    fs::FS& portalFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    const char DEFAULT_PAGE[] PROGMEM =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MEGAWARE</title></head>"
        "<body style='background:#000;color:#39ff88;font-family:monospace;"
        "text-align:center;padding:48px 20px'>"
        "<h1>MEGAWARE</h1><p>captive portal is running</p>"
        "<p style='color:#1a733d'>Select a page from SD/Flash in the<br>"
        "Portal feature to replace this stub.</p>"
        "</body></html>";

    void servePage() {
        if (pagePath[0]) {
            File f = portalFs().open(pagePath, FILE_READ);
            if (f) {
                server->setContentLength(f.size());
                server->send(200, "text/html", "");
                uint8_t buf[512];
                while (f.available()) {
                    int n = f.read(buf, sizeof(buf));
                    if (n <= 0) break;
                    server->sendContent((const char*)buf, n);
                }
                f.close();
            } else {
                server->send(200, "text/html", DEFAULT_PAGE);
            }
        } else {
            server->send(200, "text/html", DEFAULT_PAGE);
        }
        hitCount++;
    }
}

namespace Portal {

bool start(const String& page) {
    if (running) stop();

    // забираем :80 и :53 — WebUI увидит клейм и сам уйдёт с портов
    if (!ResMgr::claim(ResMgr::Resource::Http80, "portal")) return false;
    if (!ResMgr::claim(ResMgr::Resource::Dns53, "portal")) {
        ResMgr::release(ResMgr::Resource::Http80, "portal");
        return false;
    }
    WebUI::stop(); // освободить порт немедленно (не ждём следующий тик)

    if (!WifiMgr::isAPRunning()) WifiMgr::startAP(0);

    server = new WebServer(80);
    server->on("/", servePage);
    server->onNotFound(servePage); // captive: ЛЮБОЙ URI отдаёт страницу
    server->begin();

    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP()); // все имена -> нас

    if (page.length()) {
        snprintf(pagePath, sizeof(pagePath), "%s", page.c_str());
        String nm = page.substring(page.lastIndexOf('/') + 1);
        snprintf(pageNameStr, sizeof(pageNameStr), "%s", nm.c_str());
    } else {
        pagePath[0] = 0;
        snprintf(pageNameStr, sizeof(pageNameStr), "(built-in)");
    }
    running = true;
    return true;
}

void stop() {
    if (!running) return;
    if (dns) { dns->stop(); delete dns; dns = nullptr; }
    if (server) { server->stop(); delete server; server = nullptr; }
    ResMgr::release(ResMgr::Resource::Http80, "portal");
    ResMgr::release(ResMgr::Resource::Dns53, "portal");
    running = false;
}

bool isRunning() { return running; }

void loop() {
    if (!running) return;
    if (dns) dns->processNextRequest();
    if (server) server->handleClient();
}

uint32_t hits() { return hitCount; }

const char* pageName() { return pageNameStr; }

} // namespace Portal
