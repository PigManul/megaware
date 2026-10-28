-- MEGAWARE Snake (Lua)
-- Управление: вращай энкодер — поворот змейки (по часовой = направо,
-- против часовой = налево). Короткий клик Quit — выход из игры.

local CELL = 10
local COLS = math.floor(320 / CELL)
local ROWS = math.floor(170 / CELL)

local dirs = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} }

local snake = { {x = 8, y = 8}, {x = 7, y = 8}, {x = 6, y = 8} }
local dir = 2
local score = 0

local function randFood()
    while true do
        local fx = math.random(0, COLS - 1)
        local fy = math.random(0, ROWS - 1)
        local hit = false
        for i = 1, #snake do
            if snake[i].x == fx and snake[i].y == fy then hit = true end
        end
        if not hit then return {x = fx, y = fy} end
    end
end

local food = randFood()

local function draw()
    gfx.clear(6, 2, 4)
    gfx.rect(food.x * CELL, food.y * CELL, CELL, CELL, 90, 220, 120)
    for i = 1, #snake do
        local seg = snake[i]
        if i == #snake then
            gfx.rect(seg.x * CELL + 1, seg.y * CELL + 1, CELL - 2, CELL - 2, 255, 60, 70)
        else
            gfx.rect(seg.x * CELL + 1, seg.y * CELL + 1, CELL - 2, CELL - 2, 214, 24, 34)
        end
    end
    gfx.text(4, 156, "Score: " .. score, 200, 180, 180)
end

gfx.clear(6, 2, 4)
gfx.text(88, 68, "MEGAWARE SNAKE", 214, 24, 34)
gfx.text(58, 92, "rotate: turn   quit: exit", 150, 110, 112)
sleep(1200)

while true do
    input.update()
    local d = input.dx()
    if d > 0 then dir = (dir % 4) + 1 end
    if d < 0 then dir = ((dir - 2) % 4) + 1 end

    local head = snake[#snake]
    local nx = head.x + dirs[dir][1]
    local ny = head.y + dirs[dir][2]

    if nx < 0 or nx >= COLS or ny < 0 or ny >= ROWS then break end

    local selfHit = false
    for i = 1, #snake do
        if snake[i].x == nx and snake[i].y == ny then selfHit = true end
    end
    if selfHit then break end

    table.insert(snake, {x = nx, y = ny})

    if nx == food.x and ny == food.y then
        score = score + 1
        food = randFood()
    else
        table.remove(snake, 1)
    end

    draw()
    sleep(140)
end

gfx.clear(6, 2, 4)
gfx.text(108, 72, "GAME OVER", 255, 64, 64)
gfx.text(96, 96, "Score: " .. score, 200, 180, 180)
sleep(2000)
