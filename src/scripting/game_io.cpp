#include "game_io.h"
#include "../ui/ui.h"
#include "../net/webui.h"
#include "../../include/theme.h"

namespace {
    int accumDx = 0;
    bool clickFlag = false;
    bool quitFlag = false;
}

void GameIO::clear(uint8_t r, uint8_t g, uint8_t b) { ui.rawDisplay().fillScreen(RGB565(r, g, b)); }
void GameIO::rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) { ui.rawDisplay().fillRect(x, y, w, h, RGB565(r, g, b)); }
void GameIO::circle(int x, int y, int r, uint8_t cr, uint8_t cg, uint8_t cb) { ui.rawDisplay().fillCircle(x, y, r, RGB565(cr, cg, cb)); }

void GameIO::text(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b) {
    ui.rawDisplay().setTextColor(RGB565(r, g, b));
    ui.rawDisplay().setTextDatum(textdatum_t::top_left);
    ui.rawDisplay().setFont(&fonts::Font2);
    ui.rawDisplay().drawString(s, x, y);
}

InputEvent GameIO::poll() {
    if (WebUI::isRunning()) WebUI::loop(); // обслуживаем веб, пока скрипт "спит"

    InputState in = Board::pollInput();
    accumDx += in.rotationSteps;
    if (in.event == InputEvent::Click) clickFlag = true;
    if (in.event == InputEvent::UserKey) quitFlag = true;
    return in.event;
}

int GameIO::dx() { int v = accumDx; accumDx = 0; return v; }
bool GameIO::click() { bool v = clickFlag; clickFlag = false; return v; }
bool GameIO::quit() { bool v = quitFlag; quitFlag = false; return v; }

int GameIO::screenWidth()  { return 320; }
int GameIO::screenHeight() { return 170; }
