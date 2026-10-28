#include "memory_controller.h"
#include <esp_heap_caps.h>

namespace {
    uint32_t lowWatermark = UINT32_MAX;
}

void MemoryController::begin() {
    lowWatermark = ESP.getFreeHeap();
}

void MemoryController::sample() {
    const uint32_t free = ESP.getFreeHeap();
    if (free < lowWatermark) lowWatermark = free;
}

uint32_t MemoryController::freeHeap() { return ESP.getFreeHeap(); }
uint32_t MemoryController::minFreeHeap() { return lowWatermark; }
uint32_t MemoryController::freePsram() { return ESP.getFreePsram(); }
uint32_t MemoryController::largestBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
uint32_t MemoryController::fragmentationPct() {
    const uint32_t free = ESP.getFreeHeap();
    const uint32_t largest = largestBlock();
    return free ? 100U - (largest * 100U / free) : 0;
}
