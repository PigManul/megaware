// =====================================================================
//  Wi-Fi: Pcap Log — passive 802.11 capture straight to an SD file in
//  pcap format (open in Wireshark). Click: start/stop (new file per
//  session: /wifi/cap_<ms>.pcap). Rotation: channel 1-13.
//  Research artifact generator: passive RX only, never transmits.
// =====================================================================
#include "../ui/features.h"
#include "../net/pcap_logger.h"
#include "../../include/theme.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>

namespace {
    bool recording = false;
    uint8_t channel = 1;
    uint32_t lastPktAt = 0;
    uint32_t pps = 0;
    uint32_t winStart = 0;
    uint32_t winPkts = 0;
    uint32_t mgmtPkts = 0;
    uint32_t dataPkts = 0;
    char statusMsg[48] = "";

    void rx(void* buf, wifi_promiscuous_pkt_type_t type) {
        wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
        if (PcapLogger::active()) PcapLogger::writeFrame(pkt->payload, pkt->rx_ctrl.sig_len);
        if (type == WIFI_PKT_MGMT) mgmtPkts++;
        if (type == WIFI_PKT_DATA) dataPkts++;
        winPkts++;
        lastPktAt = millis();
        if (millis() - winStart >= 1000) {
            pps = winPkts;
            winPkts = 0;
            winStart = millis();
            Features::requestRedraw();
        }
    }

    bool startRec() {
        WiFi.mode(WIFI_STA);
        if (!SD.exists("/wifi")) SD.mkdir("/wifi");
        String path = "/wifi/cap_" + String(millis()) + ".pcap";
        if (!PcapLogger::start(path)) {
            snprintf(statusMsg, sizeof(statusMsg), "SD write failed");
            return false;
        }
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&rx);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        recording = true;
        statusMsg[0] = 0;
        return true;
    }

    void stopRec() {
        esp_wifi_set_promiscuous(false);
        PcapLogger::stop();
        recording = false;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "PCAP LOGGER");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        char line[52];
        if (!recording) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("passive capture to SD, pcap format", x0 + 10, y0 + 24);
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("click: start (ch " + String(channel) + ")", x0 + 10, y0 + 44);
        } else {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString("RECORDING  ch " + String(channel) + "  " + String(pps) + " pkt/s", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT, Theme::BG);
            snprintf(line, sizeof(line), "frames %lu", (unsigned long)PcapLogger::frames());
            c.drawString(line, x0 + 10, y0 + 44);
            snprintf(line, sizeof(line), "mgmt %lu  data %lu", (unsigned long)mgmtPkts, (unsigned long)dataPkts);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 62);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            snprintf(line, sizeof(line), "%s", PcapLogger::filename());
            c.drawString(line, x0 + 10, y0 + 84);
        }
        if (statusMsg[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 30);
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString(recording ? "rot: channel  click: stop" : "rot: channel  click: start", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            channel = ((channel - 1 + in.rotationSteps) % 13 + 13) % 13 + 1;
            if (recording) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (recording) stopRec();
            else startRec();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void onExit() { if (recording) stopRec(); }
}

REGISTER_FEATURE("Pcap Log", MwCat::Wifi, MwIcon::Pulse, 55,
                 nullptr, input, nullptr, draw, onExit);
