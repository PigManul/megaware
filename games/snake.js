// =====================================================================
//  MEGAWARE test game: SNAKE (JS / Elk)
//
//  Заливается через WebUI (например, в /games), запуск: Files -> snake.js.
//  На SD по умолчанию не попадает — это просто файл в репозитории.
//
//  Управление:  вращение — поворот на 90°,
//               клик — пауза / рестарт после проигрыша,
//               Quit — выход из игры.
//
//  API прошивки: gfx.*, input.*, led.*, sys.*, print() — см. README.
//  Нюансы Elk: объекта Math нет (свой ГПСЧ через sys.millis()), нет
//  побитовых операций и объявлений var — только let и функциональные
//  выражения. sleep() обрывает скрипт по Quit — ожидание кадра через
//  активный опрос input.update() (он же обслуживает WebUI).
// =====================================================================

let CELL = 10;
let COLS = 32;          // 320 / CELL
let ROWS = 16;          // 160 / CELL, снизу строка счёта
let MAXLEN = 256;

// палитра Terminal-Lite
let ACC_R = 0x39, ACC_G = 0xFF, ACC_B = 0x88;
let DIM_R = 0x1a, DIM_G = 0x73, DIM_B = 0x3d;

// тело змеи: [0] — хвост ... [len-1] — голова
let snakeX = [], snakeY = [];
for (let z = 0; z < MAXLEN; z++) { snakeX[z] = 0; snakeY[z] = 0; }

let len, dir, fx, fy, score, speed, paused, dead;
let seed = 1;

let rnd = function (maxv) {                 // Lehmer LCG — Math в Elk нет
  seed = (seed * 16807) % 2147483647;
  return seed % maxv;
};

let occupied = function (x, y) {
  for (let i = 0; i < len; i++) {
    if (snakeX[i] == x && snakeY[i] == y) return true;
  }
  return false;
};

let placeFood = function () {
  let x = 0, y = 0;
  do {
    x = rnd(COLS);
    y = rnd(ROWS);
  } while (occupied(x, y));
  fx = x; fy = y;
};

let reset = function () {
  len = 3;
  snakeX[0] = 6; snakeY[0] = 8;
  snakeX[1] = 7; snakeY[1] = 8;
  snakeX[2] = 8; snakeY[2] = 8;
  dir = 1;
  score = 0; speed = 120; paused = false; dead = false;
  placeFood();
};

let applyTurn = function () {
  let d = input.dx();
  while (d != 0) {
    dir = (dir + (d > 0 ? 1 : 3)) % 4;      // CW -> поворот вправо
    d = d - (d > 0 ? 1 : -1);
  }
};

let shiftBody = function (hx, hy, grow) {
  let last = grow ? len : len - 1;
  for (let i = last; i > 0; i--) {
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  snakeX[0] = hx; snakeY[0] = hy;
  if (grow) len = len + 1;
};

let step = function () {
  let hx = snakeX[len - 1], hy = snakeY[len - 1];
  if (dir == 0) hy = hy - 1;
  else if (dir == 1) hx = hx + 1;
  else if (dir == 2) hy = hy + 1;
  else hx = hx - 1;
  if (hx < 0 || hy < 0 || hx >= COLS || hy >= ROWS) { dead = true; return; }
  for (let i = 0; i < len - 1; i++) {
    if (snakeX[i] == hx && snakeY[i] == hy) { dead = true; return; }
  }
  let grow = (hx == fx && hy == fy);
  shiftBody(hx, hy, grow);
  if (grow) {
    score = score + 1;
    if (speed > 50) speed = speed - 5;
    if (len < MAXLEN) {
      led.setColor(ACC_R, ACC_G, ACC_B);    // вспышка на съеденной еде
      placeFood();
    }
  }
};

let draw = function () {
  gfx.clear(0, 0, 0);
  for (let i = 0; i < len; i++) {
    gfx.rect(snakeX[i] * CELL, snakeY[i] * CELL, CELL - 1, CELL - 1, ACC_R, ACC_G, ACC_B);
  }
  gfx.rect(fx * CELL, fy * CELL, CELL - 1, CELL - 1, DIM_R, DIM_G, DIM_B);
  gfx.text(2, 162, "SCORE " + score + "   SPEED " + speed, DIM_R, DIM_G, DIM_B);
  if (paused) {
    gfx.text(140, 78, "PAUSE", ACC_R, ACC_G, ACC_B);
  }
  if (dead) {
    gfx.text(116, 64, "GAME OVER", 0xff, 0x50, 0x50);
    gfx.text(64, 82, "click: restart   quit: exit", DIM_R, DIM_G, DIM_B);
  }
};

reset();
seed = sys.millis() % 2147483646 + 1;
led.setMode(1);   // Solid: фон игры не «дышит»
led.setColor(DIM_R, DIM_G, DIM_B);

while (true) {
  input.update();
  applyTurn();
  if (input.quit()) break;
  if (input.click()) {
    if (dead) reset();
    else paused = !paused;
  }

  if (!paused && !dead) {
    // ждём тик игры, опрашивая ввод (без sleep: Quit должен выходить чисто)
    let started = sys.millis();
    while (sys.millis() - started < speed) {
      input.update();
      applyTurn();
      if (input.quit()) break;
    }
    if (input.quit()) break;
    step();
  } else {
    input.update();
  }
  draw();
}

gfx.clear(0, 0, 0);
led.setMode(2);   // вернуть «дыхание»
print("snake(js): exit, score " + score);
