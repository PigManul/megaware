#include "wifi_manager.h"
#include "../hal/config.h"
#include <ESPmDNS.h>

namespace {
    WifiConnState state = WifiConnState::Idle;
    uint32_t connectStartedAt = 0;
    constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
    String pendingSSID;
    String pendingPass; // для сохранения кредов в NVS после успешного подключения
    bool radioStarted = false;
    bool apRunning = false;
    String autoSSID, autoPass;
    bool autoConnectPending = false;
    bool mdnsRunning = false;

    // ВАЖНО про память: WiFi.mode(WIFI_STA) реально поднимает
    // lwIP/Wi-Fi-стек — десятки КБ heap, даже если Wi-Fi ни разу не
    // использовался. Радио трогается только при первом РЕАЛЬНОМ
    // использовании (скан/подключение/AP/автоподключение по сохранённой
    // сети) — если раздел Wi-Fi вообще не открывали, память свободна.
    void ensureRadioStarted() {
        if (radioStarted) return;
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        radioStarted = true;
    }
}

void WifiMgr::begin() {
    // Пусто намеренно — см. ensureRadioStarted().
}

void WifiMgr::tryAutoConnect() {
    if (autoConnectPending || state == WifiConnState::Connecting || isConnected()) return;
    String ssid = Config::getString("wifi_ssid", "");
    if (ssid.length() == 0) return; // ничего не сохранено — радио не трогаем вообще
    String pass = Config::getString("wifi_pass", "");
    autoSSID = ssid;
    autoPass = pass;
    autoConnectPending = true;
    startScan();
}

void WifiMgr::startScan() {
    ensureRadioStarted();
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
}

bool WifiMgr::isScanning() { return WiFi.scanComplete() == WIFI_SCAN_RUNNING; }

int WifiMgr::resultCount() {
    int n = WiFi.scanComplete();
    return n > 0 ? n : 0;
}

WifiNetwork WifiMgr::result(int index) {
    WifiNetwork n;
    n.ssid = WiFi.SSID(index);
    n.rssi = WiFi.RSSI(index);
    n.channel = WiFi.channel(index);
    n.secure = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
    return n;
}

void WifiMgr::connect(const String& ssid, const String& password) {
    ensureRadioStarted();
    autoConnectPending = false;
    if (state == WifiConnState::Connecting || isConnected()) WiFi.disconnect(false, false);
    // Асинхронное сканирование хранит список сетей в heap до scanDelete().
    // После выбора сети он больше не нужен.
    pendingSSID = ssid;
    pendingPass = password;
    state = WifiConnState::Connecting;
    connectStartedAt = millis();
    WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);
}

void WifiMgr::disconnect() {
    WiFi.disconnect(true);
    state = WifiConnState::Idle;
}

void WifiMgr::releaseIdleResources() {
    if (isConnected() || apRunning) return;
    autoConnectPending = false;
    WiFi.scanDelete();
    WiFi.disconnect(true, false);
    if (mdnsRunning) { MDNS.end(); mdnsRunning = false; } // иначе задача mDNS висит с выключенным радио
    WiFi.mode(WIFI_OFF);
    radioStarted = false;
    state = WifiConnState::Idle;
    pendingSSID = String();
    pendingPass = String();
    autoSSID = String();
    autoPass = String();
}

WifiConnState WifiMgr::connState() {
    if (autoConnectPending && !isScanning()) {
        autoConnectPending = false;
        int count = resultCount();
        for (int i = 0; i < count; ++i) {
            if (WiFi.SSID(i) == autoSSID) {
                connect(autoSSID, autoPass);
                break;
            }
        }
    }
    if (state == WifiConnState::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            state = WifiConnState::Connected;
            if (!mdnsRunning) {
                mdnsRunning = MDNS.begin("megaware");
                if (mdnsRunning) MDNS.addService("http", "tcp", 80);
            }
            // Успешное подключение — запоминаем сеть И пароль, чтобы
            // автоматически подключаться при следующей загрузке
            // (tryAutoConnect читает оба ключа; пароль критичен для
            // защищённых сетей).
            Config::setString("wifi_ssid", pendingSSID);
            Config::setString("wifi_pass", pendingPass);
        } else if (millis() - connectStartedAt > CONNECT_TIMEOUT_MS) {
            state = WifiConnState::Failed;
        }
    } else if (state == WifiConnState::Connected && WiFi.status() != WL_CONNECTED) {
        state = WifiConnState::Idle;
    }
    return state;
}

String WifiMgr::currentSSID() {
    return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : pendingSSID;
}

int WifiMgr::rssiPercent(int32_t rssiDbm) {
    int pct = (int)((rssiDbm + 100) * 2);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

bool WifiMgr::isConnected() { return WiFi.status() == WL_CONNECTED; }

const char* WifiMgr::hostname() { return "megaware.local"; }

void WifiMgr::startAP(int preset) {
    // ВАЖНО (реальный баг): WiFi.softAPConfig() на УЖЕ запущенной точке
    // доступа часто просто не применяется — ESP32 требует полной
    // остановки интерфейса перед сменой адреса, иначе молча продолжает
    // работать на старом. Поэтому сначала гасим AP, если он уже поднят.
    if (apRunning) {
        WiFi.softAPdisconnect(true);
        apRunning = false;
        delay(100);
    }

    IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
    (void)preset; // фиксированный адрес
    WiFi.mode(isConnected() ? WIFI_AP_STA : WIFI_AP);
    radioStarted = true;
    delay(20);
    const bool configured = WiFi.softAPConfig(ip, gw, mask);
    const bool started = WiFi.softAP("MEGAWARE", "megaware123");
    apRunning = configured && started;
    if (apRunning && !mdnsRunning) {
        mdnsRunning = MDNS.begin("megaware");
        if (mdnsRunning) MDNS.addService("http", "tcp", 80);
    }
}

void WifiMgr::stopAP() {
    WiFi.softAPdisconnect(true);
    apRunning = false;
    if (!isConnected() && mdnsRunning) { MDNS.end(); mdnsRunning = false; }
}

bool WifiMgr::isAPRunning() { return apRunning; }
String WifiMgr::apIP() { return WiFi.softAPIP().toString(); }
