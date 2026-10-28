-- =====================================================================
--  MEGAWARE test game: SNAKE (Lua)
--
--  Заливается через WebUI (например, в /games), запуск: Files -> snake.lua.
--  На SD по умолчанию не попадает — это просто файл в репозитории.
--
--  Управление:  вращение — поворот на 90°,
--               клик — пауза / рестарт после проигрыша,
--               Quit — выход из игры.
--
--  API прошивки: gfx.*, input.*, led.*, sys.*, print() — см. README.
--  Без sleep(): sleep() обрывает скрипт по Quit, поэтому кадрос
--  держим активным ожиданием через input.update() — он же обслуживает
--  WebUI в фоне (см. src/scripting/game_io.cpp).
-- =====================================================================

local CELL = 10
local COLS = 32          -- 320 / CELL
local ROWS = 16          -- 160 / CELL, снизу строка счёта
local DIRS = {{0,-1},{1,0},{0,1},{-1,0}}  -- 0 up, 1 right, 2 down, 3 left

-- палитра Terminal-Lite
local ACC = {0x39, 0xFF, 0x88}
local DIM = {0x1a, 0x73, 0x3d}

local snake, dir, food, score, speed, paused, dead

local function placeFood()
  while true do
    local fx = math.random(0, COLS - 1)
    local fy = math.random(0, ROWS - 1)
    local free = true
    for i = 1, #snake do
      if snake[i].x == fx and snake[i].y == fy then free = false; break end
    end
    if free then food = {x = fx, y = fy}; return end
  end
end

local function reset()
  snake = {{x = 8, y = 8}, {x = 7, y = 8}, {x = 6, y = 8}}
  dir = 1
  score = 0
  speed = 120
  paused = false
  dead = false
  placeFood()
end

local function applyTurn()
  local d = input.dx()
  while d ~= 0 do
    dir = (dir + (d > 0 and 1 or 3)) % 4   -- CW -> поворот вправо
    d = d - (d > 0 and 1 or -1)
  end
end

local function step()
  local head = snake[#snake]
  local nx = head.x + DIRS[dir + 1][1]
  local ny = head.y + DIRS[dir + 1][2]
  if nx < 0 or ny < 0 or nx >= COLS or ny >= ROWS then dead = true; return end
  for i = 1, #snake - 1 do
    if snake[i].x == nx and snake[i].y == ny then dead = true; return end
  end
  table.insert(snake, {x = nx, y = ny})
  if nx == food.x and ny == food.y then
    score = score + 1
    if speed > 50 then speed = speed - 5 end
    led.setColor(ACC[1], ACC[2], ACC[3])   -- вспышка на съеденной еде
    placeFood()
  else
    table.remove(snake, 1)
  end
end

local function draw()
  gfx.clear(0, 0, 0)
  for i = 1, #snake do
    gfx.rect(snake[i].x * CELL, snake[i].y * CELL, CELL - 1, CELL - 1, ACC[1], ACC[2], ACC[3])
  end
  gfx.rect(food.x * CELL, food.y * CELL, CELL - 1, CELL - 1, DIM[1], DIM[2], DIM[3])
  gfx.text(2, 162, "SCORE " .. score .. "   SPEED " .. speed, DIM[1], DIM[2], DIM[3])
  if paused then
    gfx.text(140, 78, "PAUSE", ACC[1], ACC[2], ACC[3])
  end
  if dead then
    gfx.text(116, 64, "GAME OVER", 0xff, 0x50, 0x50)
    gfx.text(64, 82, "click: restart   quit: exit", DIM[1], DIM[2], DIM[3])
  end
end

math.randomseed(sys.millis())
reset()
led.setMode(1)   -- Solid: фон игры не «дышит»
led.setColor(DIM[1], DIM[2], DIM[3])

while true do
  input.update()
  applyTurn()
  if input.quit() then break end
  if input.click() then
    if dead then reset() else paused = not paused end
  end

  if not paused and not dead then
    -- ждём тик игры, опрашивая ввод каждые ~2 мс
    local started = sys.millis()
    while sys.millis() - started < speed do
      input.update()
      applyTurn()
      if input.quit() then break end
    end
    if input.quit() then break end
    step()
  else
    input.update()
  end
  draw()
end

gfx.clear(0, 0, 0)
led.setMode(2)   -- вернуть «дыхание»
print("snake(lua): exit, score " .. score)
