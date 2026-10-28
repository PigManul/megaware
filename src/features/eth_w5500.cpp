// =====================================================================
//  Ethernet: W5500 — проводная сеть через внешний модуль в Grove-порту
//  (CS=44, IRQ=43, SPI 9/10/11, отдельный SPI-контроллер). Клик —
//  поднять интерфейс (DHCP). WebUI/API после этого работают и по кабелю.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <esp_eth.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <SPI.h>
#include <driver/spi_master.h>

namespace {
    bool attempted = false;
    bool ethUp = false;
    esp_netif_t* ethNetif = nullptr;
    esp_eth_handle_t ethHandle = nullptr;
    char statusMsg[56] = "";

    bool ethStart() {
        if (ethNetif) return true;
        // инфраструктура netif (уже может быть создана Wi-Fi — идемпотентно)
        esp_netif_init();
        esp_event_loop_create_default();

        // W5500 на СВОЁМ SPI-контроллере (HSPI/SPI3), пины Grove по GPIO-matrix
        spi_bus_config_t bus = {};
        bus.mosi_io_num = PIN_SPI_MOSI;
        bus.miso_io_num = PIN_SPI_MISO;
        bus.sclk_io_num = PIN_SPI_SCK;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK &&
            spi_bus_initialize(SPI3_HOST, &bus, 1) != ESP_OK) {
            snprintf(statusMsg, sizeof(statusMsg), "SPI3 busy?");
            return false;
        }
        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 12000000;
        dev.mode = 0;
        dev.spics_io_num = PIN_W5500_CS;
        dev.queue_size = 20;
        dev.command_bits = 16;
        dev.address_bits = 8;
        spi_device_handle_t spiDev;
        if (spi_bus_add_device(SPI3_HOST, &dev, &spiDev) != ESP_OK) {
            snprintf(statusMsg, sizeof(statusMsg), "spi device failed");
            return false;
        }

        eth_w5500_config_t wcfg = ETH_W5500_DEFAULT_CONFIG(spiDev);
        // int_gpio_num = -1: polling-режим. IRQ платы сидит на GPIO43 —
        // это UART0 TX, консоль дёргает линию -> interrupt storm -> crash
        wcfg.int_gpio_num = -1;
        eth_mac_config_t mcfg = ETH_MAC_DEFAULT_CONFIG();
        esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&wcfg, &mcfg);
        if (!mac) { snprintf(statusMsg, sizeof(statusMsg), "mac init failed"); return false; }
        eth_phy_config_t pcfg = ETH_PHY_DEFAULT_CONFIG();
        pcfg.autonego_timeout_ms = 300;
        esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&pcfg);
        if (!phy) { snprintf(statusMsg, sizeof(statusMsg), "phy init failed"); return false; }

        esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac, phy);
        if (esp_eth_driver_install(&ecfg, &ethHandle) != ESP_OK) {
            snprintf(statusMsg, sizeof(statusMsg), "driver install failed");
            return false;
        }
        esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
        ethNetif = esp_netif_new(&ncfg);
        if (!ethNetif) { snprintf(statusMsg, sizeof(statusMsg), "netif failed"); return false; }
        esp_netif_attach(ethNetif, esp_eth_new_netif_glue(ethHandle));
        esp_eth_start(ethHandle);
        ethUp = true;
        return true;
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "W5500 ETHERNET");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (!ethNetif) {
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString("external module in Grove port", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("CS 44  IRQ 43  SPI 9/10/11", x0 + 10, y0 + 42);
            if (statusMsg[0]) {
                c.setTextColor(Theme::ERR, Theme::BG);
                c.drawString(statusMsg, x0 + 10, y0 + 62);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: start (DHCP)", x0 + 10, y0 + h - 14);
            return;
        }

        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(ethNetif, &ip);
        bool gotIp = ip.ip.addr != 0;
        c.setTextColor(gotIp ? Theme::OK : Theme::WARN, Theme::BG);
        c.drawString(gotIp ? "LINK UP (DHCP)" : "started, waiting for DHCP/cable...", x0 + 10, y0 + 26);
        if (gotIp) {
            char line[48];
            snprintf(line, sizeof(line), "IP   %u.%u.%u.%u", (unsigned)(ip.ip.addr & 0xFF),
                     (unsigned)((ip.ip.addr >> 8) & 0xFF), (unsigned)((ip.ip.addr >> 16) & 0xFF),
                     (unsigned)((ip.ip.addr >> 24) & 0xFF));
            c.setTextColor(Theme::TEXT, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 46);
            snprintf(line, sizeof(line), "GW   %u.%u.%u.%u", (unsigned)(ip.gw.addr & 0xFF),
                     (unsigned)((ip.gw.addr >> 8) & 0xFF), (unsigned)((ip.gw.addr >> 16) & 0xFF),
                     (unsigned)((ip.gw.addr >> 24) & 0xFF));
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            c.drawString(line, x0 + 10, y0 + 62);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("WebUI/API work over cable too", x0 + 10, y0 + h - 14);
        } else if (statusMsg[0]) {
            c.setTextColor(Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + 46);
        }
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::Click) {
            attempted = true;
            ethStart();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        static uint32_t last = 0;
        if (ethNetif && millis() - last > 2000) {
            last = millis();
            Features::requestRedraw();
        }
    }
}

REGISTER_FEATURE("W5500", "Ethernet", MwIcon::Ethernet, 10,
                 nullptr, input, tick, draw, nullptr);
