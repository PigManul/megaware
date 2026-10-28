#pragma once
#include <Arduino.h>
#include "../hal/board.h"

// =====================================================================
//  GameIO — минимальный "холст + джойстик" для скриптов (Lua/JS).
//  Рисует ПРЯМО на физический дисплей (в обход спрайтового буфера Ui).
//
//  ВАЖНО про poll(): это ЕДИНАЯ точка опроса энкодера/кнопок для всего
//  скриптинга. Раньше sleep() опрашивал Board::pollInput() напрямую и
//  просто выбрасывал накопленную дельту — вращение "терялось" между
//  кадрами игры. Теперь ЛЮБОЙ опрос идёт через poll() и копится в общий
//  буфер. poll() заодно обслуживает WebUI::loop() — иначе веб-интерфейс
//  "замирает" на всё время выполнения скрипта (главный цикл блокирован
//  внутри runFile()).
// =====================================================================

namespace GameIO {
    void clear(uint8_t r, uint8_t g, uint8_t b);
    void rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
    void circle(int x, int y, int r, uint8_t cr, uint8_t cg, uint8_t cb);
    void text(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b);

    InputEvent poll();

    int  dx();
    bool click();
    bool quit();

    int screenWidth();
    int screenHeight();
}
