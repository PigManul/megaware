#include "audio_out.h"
#include "../../include/pins.h"
#include <driver/i2s.h>

namespace {
    bool installed = false;
    uint32_t curRate = 0;
}

namespace AudioOut {

bool begin(uint32_t sampleRateHz) {
    if (installed) {
        if (curRate == sampleRateHz) return true;
        if (i2s_set_sample_rates(I2S_NUM_1, sampleRateHz) == ESP_OK) { curRate = sampleRateHz; return true; }
        i2s_driver_uninstall(I2S_NUM_1);
        installed = false;
    }
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sampleRateHz;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_SPK_BCLK;
    pins.ws_io_num = PIN_SPK_LRCLK;
    pins.data_out_num = PIN_SPK_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_1);
    installed = true;
    curRate = sampleRateHz;
    return true;
}

bool write(const int16_t* samples, int count) {
    if (!installed || count <= 0) return false;
    size_t written = 0;
    // блокирующая запись в DMA: чанки небольшие, тик успевает
    i2s_write(I2S_NUM_1, samples, count * sizeof(int16_t), &written, pdMS_TO_TICKS(60));
    return written > 0;
}

void stop() {
    if (installed) i2s_zero_dma_buffer(I2S_NUM_1);
}

bool active() { return installed; }

} // namespace AudioOut
