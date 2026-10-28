#pragma once
#include <Arduino.h>

namespace JsEngine {
    bool begin();
    bool runFile(const String& path);
    String lastError();
}
