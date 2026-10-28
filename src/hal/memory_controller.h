#pragma once
#include <Arduino.h>

namespace MemoryController {
    void begin();
    void sample();
    uint32_t freeHeap();
    uint32_t minFreeHeap();
    uint32_t freePsram();
    uint32_t largestBlock();
    uint32_t fragmentationPct();
}
