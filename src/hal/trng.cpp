#include "trng.h"
#include <esp_system.h>
namespace Trng {
    uint32_t rng32() { return esp_random(); }
    void rngBytes(uint8_t* out, int len) {
        for (int i = 0; i < len; i += 4) {
            uint32_t r = esp_random();
            memcpy(out + i, &r, (len - i >= 4) ? 4 : (len - i));
        }
    }
}
