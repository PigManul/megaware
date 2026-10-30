#pragma once
#include <Arduino.h>
#include <esp_netif.h>

// =====================================================================
//  EthW5500 — shared W5500 bring-up used by both Ethernet features
//  (the plain W5500 screen and the TCP bridge). One module, one SPI2
//  device, one netif: entering a second Ethernet feature cannot create
//  a duplicate driver.
//  Module presence is probed first (version register 0x0039 == 0x04
//  over register-level SPI): absent hardware must never hang the UI.
// =====================================================================

namespace EthW5500 {
    bool probe();              // quick version-register check (no esp_eth)
    bool start();              // probe + SPI2 + driver + netif + poll mode
    void stop();               // esp_eth_stop (driver stays installed)
    bool running();            // start() succeeded and not stopped
    bool hasIP();              // DHCP/client address acquired
    esp_netif_t* netif();
    const char* lastError();
}
