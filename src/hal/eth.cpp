#include "eth.h"
#include "../../include/pins.h"
#include <SPI.h>
#include <driver/spi_master.h>
#include <esp_eth.h>
#include <esp_event.h>

namespace {
    bool inited = false;
    bool stopped = false;
    esp_netif_t* ethNetif = nullptr;
    esp_eth_handle_t ethHandle = nullptr;
    char errMsg[56] = "";

    // Quick check of the module WITHOUT esp_eth: read the version
    // register (0x0039, common block) via register-level Arduino SPI on
    // SPI2 - the same technique as NRF24. A real W5500 answers 0x04.
    // Earlier we called esp_eth right away: spi_bus_initialize(SPI3) on
    // pins 9/10/11 stole the GPIO matrix from the display (UI froze)
    // and the driver on an empty bus could spin forever.
    bool probe() {
        SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
        pinMode(PIN_W5500_CS, OUTPUT);
        digitalWrite(PIN_W5500_CS, HIGH);
        SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_W5500_CS, LOW);
        SPI.transfer(0x00); SPI.transfer(0x39); SPI.transfer(0x00); // VDM read @0x0039
        uint8_t ver = SPI.transfer(0x00);
        digitalWrite(PIN_W5500_CS, HIGH);
        SPI.endTransaction();
        return ver == 0x04;
    }
}

bool EthW5500::probe() { return ::probe(); }

bool EthW5500::start() {
    if (ethNetif && !stopped) return true;
    errMsg[0] = 0;
    if (!::probe()) {
        snprintf(errMsg, sizeof(errMsg), "no module (CS 44, SPI 9/10/11)");
        return false;
    }
    esp_netif_init();
    esp_event_loop_create_default();

    // W5500 on the SHARED SPI2 controller (like NRF24). A separate
    // SPI3 on the same pins 9/10/11 pulled the GPIO matrix from the
    // display. ESP_ERR_INVALID_STATE = bus already claimed, fine.
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_SPI_MOSI;
    bus.miso_io_num = PIN_SPI_MISO;
    bus.sclk_io_num = PIN_SPI_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    esp_err_t berr = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (berr != ESP_OK && berr != ESP_ERR_INVALID_STATE) {
        snprintf(errMsg, sizeof(errMsg), "SPI2 busy?");
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
    if (spi_bus_add_device(SPI2_HOST, &dev, &spiDev) != ESP_OK) {
        snprintf(errMsg, sizeof(errMsg), "spi device failed");
        return false;
    }

    eth_w5500_config_t wcfg = ETH_W5500_DEFAULT_CONFIG(spiDev);
    // int_gpio_num = -1: polling mode. The board IRQ sits on GPIO43 -
    // that is UART0 TX, the console toggles the line -> interrupt storm.
    wcfg.int_gpio_num = -1;
    eth_mac_config_t mcfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&wcfg, &mcfg);
    if (!mac) { snprintf(errMsg, sizeof(errMsg), "mac init failed"); return false; }
    eth_phy_config_t pcfg = ETH_PHY_DEFAULT_CONFIG();
    pcfg.autonego_timeout_ms = 300;
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&pcfg);
    if (!phy) { snprintf(errMsg, sizeof(errMsg), "phy init failed"); return false; }

    esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&ecfg, &ethHandle) != ESP_OK) {
        snprintf(errMsg, sizeof(errMsg), "driver install failed");
        return false;
    }
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif = esp_netif_new(&ncfg);
    if (!ethNetif) { snprintf(errMsg, sizeof(errMsg), "netif failed"); return false; }
    esp_netif_attach(ethNetif, esp_eth_new_netif_glue(ethHandle));
    esp_eth_start(ethHandle);
    stopped = false;
    inited = true;
    return true;
}

void EthW5500::stop() {
    if (!inited || stopped) return;
    esp_eth_stop(ethHandle);
    stopped = true;
}

bool EthW5500::running() { return inited && !stopped; }
bool EthW5500::hasIP() {
    if (!ethNetif) return false;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ethNetif, &ip);
    return ip.ip.addr != 0;
}
esp_netif_t* EthW5500::netif() { return ethNetif; }
const char* EthW5500::lastError() { return errMsg; }
