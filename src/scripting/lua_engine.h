#pragma once
#include <Arduino.h>

namespace LuaEngine {
    bool begin();
    bool runFile(const String& path);
    String lastError();
}
