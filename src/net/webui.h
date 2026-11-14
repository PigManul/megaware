#pragma once
#include <Arduino.h>

// =====================================================================
//  WebUI — file manager (SD+LittleFS) with login, file editing,
//  file/folder creation, and System Info / Settings / About pages.
//  Dark neon-red design with sidebar navigation (as in the provided
//  reference). HTML is served STREAMED (server->sendContent() chunks)
//  instead of being assembled into one big String — peak memory usage
//  during page rendering is therefore much lower.
// =====================================================================

namespace WebUI {
    void start();
    void stop();
    bool isRunning();
    void loop();

    // Shared terminal-command engine: runs one command (the same set as
    // the WebUI terminal) and returns the text output. Used by the
    // Telnet server and any future remote-control transport.
    bool execCommand(const String& cmd, String& out);
}
