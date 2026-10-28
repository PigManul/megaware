# MEGAWARE: как добавить свою функцию в GUI

Этот документ самодостаточный: он рассчитан на человека, который видел
проект впервые. Нужны только установленная [PlatformIO](https://platformio.org/)
(VS Code extension или CLI) и папка этого проекта.

---

## 1. Что это за проект, в двух словах

MEGAWARE — прошивка для **LilyGo T-Embed CC1101** (ESP32-S3, 16 MB flash,
8 MB PSRAM): экран ST7789 170×320, радиомодуль CC1101 (Sub-GHz), энкодер
с кнопкой, боковая кнопка Quit, 8 штук WS2812, датчик батареи BQ27220,
слот microSD.

GUI выглядит так: **сайдбар** слева со значками разделов (Dashboard,
SubGhz, Wifi, Files, Bluetooth, Settings, …) → подвкладки сверху
(например у SubGhz: Status / Scan / Capture) → содержимое карточкой
справа. Управление:

| Действие | Результат |
|---|---|
| вращение энкодера | листает текущий уровень (разделы / вкладки / внутри фичи) |
| клик по энкодеру (OK) | вход внутрь / действие |
| короткий клик по боковой кнопке (**Quit**) | на уровень выше; **из фичи — выход** |
| длинный клик по энкодеру (LongPress) | контекстное действие (например, папка вверх в Files) |
| удержание Quit 3 секунды | выключение (deep sleep); будит кнопка энкодера |

## 2. Главная идея

**Одна функция = один файл** в `src/features/`. В этом файле функция сама
объявляет:

- название пункта и иконку;
- категорию GUI, куда её положить (новая категория создаётся автоматически);
- порядок в категории;
- код: вход, обработка ввода, цикл, отрисовка, выход.

Больше **никаких правок** по проекту: ни `main.cpp`, ни GUI, ни списков
меню, ни `platformio.ini`. Все файлы фич собирает реестр
(`src/ui/features.h` / `features.cpp`) — при старте прошивки он строит
пункты меню из зарегистрированных функций. Удалил файл — пункт исчез.

Готовые примеры в проекте: `src/features/subghz_*.cpp`,
`src/features/bluetooth_*.cpp` (это настоящие экраны SubGhz/Bluetooth,
мигрированные в реестр). Шаблон с подробными комментариями:
`src/features/_template.cpp.example`.

## 3. Пошагово: минимальная функция от нуля до железа

**Шаг 1.** Создай файл `src/features/hello.cpp` (имя любое, расширение
обязательно `.cpp`):

```cpp
// ============================================================
//  Пример: пункт "Hello" в новой категории "Tools".
//  Вращение меняет счётчик, клик сбрасывает, Quit выходит.
// ============================================================
#include "../ui/features.h"          // реестр, иконки, помощники рисования
#include "../../include/theme.h"     // палитра Theme::*

namespace {
    int counter = 0;                 // состояние фичи — обычные static-переменные

    // Обработка ввода. Вызывается ТОЛЬКО при реальном событии.
    // Верни true, если событие обработано; false — «не моё».
    bool onInput(const InputState& in) {
        if (in.rotationSteps != 0) {          // вращение: +N/-N шагов
            counter += in.rotationSteps;
            Features::requestRedraw();        // просим перерисовать кадр
            return true;
        }
        if (in.event == InputEvent::Click) {  // клик по энкодеру
            counter = 0;
            Features::requestRedraw();
            return true;
        }
        return false; // Quit не съедаем — GUI сам выведет из фичи
    }

    // Отрисовка. (x, y, w, h) — прямоугольник контента, рисуй только в нём.
    void onDraw(LGFX_Sprite& c, int x, int y, int w, int h) {
        MwDraw::card(c, x, y, w, h, "HELLO");  // карточка в стиле GUI
        c.setFont(&fonts::Font4);              // крупный шрифт
        c.setTextDatum(textdatum_t::middle_center);
        c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", counter);
        c.drawString(buf, x + w / 2, y + h / 2);

        c.setFont(&fonts::Font0);              // мелкий шрифт
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
        c.drawString("rotate: +/-  click: reset", x + 10, y + h - 14);
    }
}

// РЕГИСТРАЦИЯ — единственная обязательная строка.
// REGISTER_FEATURE(название, категория, иконка, порядок,
//                  onEnter, onInput, onTick, onDraw, onExit);
// Неиспользуемые обработчики — nullptr. Минимум нужен onDraw.
REGISTER_FEATURE("Hello", "Tools", MwIcon::Terminal, 10,
                 nullptr, onInput, nullptr, onDraw, nullptr);
```

**Шаг 2.** Собери и залей:

```bash
pio run -t upload        # сборка + прошивка по COM3 (порт задан в platformio.ini)
pio device monitor       # сериал-лог, 115200 (Ctrl+C — выход)
```

Если плата на другом порту: `pio run -t upload --upload-port COM5`.

**Шаг 3.** На устройстве: в сайдбаре внизу появится раздел **Tools** со
значком терминала (категория создалась сама, потому что `"Tools"` — новое
имя). Внутри — пункт **Hello**. Клик — вход, вращение меняет число,
клик — сброс, Quit — выход.

Вот и всё. Больше в проекте не нужно менять ничего.

## 4. REGISTER_FEATURE: полный контракт

```cpp
REGISTER_FEATURE(name, category, icon, order,
                 onEnter, onInput, onTick, onDraw, onExit);
```

| Аргумент | Тип | Обязателен | Смысл |
|---|---|---|---|
| `name` | `const char*` | да | название пункта (влезает в кнопку вкладки, шрифт мелкий) |
| `category` | `const char*` | да | см. раздел 5 |
| `icon` | `MwIcon::Id` | да | значок **новой** категории; для существующей — `MwIcon::None` |
| `order` | `int8_t` | да | порядок в категории: меньше — раньше; равные — как повезёт с компоновкой, поэтому задавай явно (0, 10, 20…) |
| `onEnter` | `void ()` | нет | вход в функцию (клик по пункту). Если `nullptr` — фича stateless, см. раздел 7 |
| `onInput` | `bool (const InputState&)` | нет | ввод; верни `true` = событие съедено |
| `onTick` | `void ()` | нет | цикл/обновление — **каждый тик `loop()`**, см. раздел 8 |
| `onDraw` | `void (LGFX_Sprite&, int x, int y, int w, int h)` | **да** | отрисовка контента |
| `onExit` | `void ()` | нет | выход из фичи — верни железо в исходное |

Все сигнатуры — в `struct Feature` в `src/ui/features.h`.

### Ввод (`InputState`, `src/hal/board.h`)

- `in.rotationSteps` — на сколько щелчков провернули энкодер (+/-).
- `in.event` — `InputEvent::Click` (короткий клик OK), `LongPress`
  (удержание OK), `UserKey` (короткий клик Quit), `None`.
- Удержание Quit (3 с) до фичи **не доходит** — это выключение, система
  обрабатывает сама. Не реагируй на `in.powerHeldMs`.

**Главное правило выхода:** если `onInput` вернул `false` для
`InputEvent::UserKey`, GUI сам выведет из фичи на уровень вкладок и
вызовет `onExit()`. То есть для выхода достаточно **не съедать** Quit —
свой код выхода писать не нужно. `LongPress` без обработки просто
игнорируется.

**Не вызывай `Board::pollInput()` сам** — ввод уже приходит в `onInput`;
лишний опрос своровал бы события у остального GUI.

### Состояние

Состояние фичи — обычные `static`-переменные внутри анонимного
namespace твоего файла. `onEnter` нужен, когда при входе надо что-то
инициализировать/выключать, а при `onExit` — возвращать (например,
остановить приём CC1101).

## 5. Категории

Категория = раздел сайдбара. **Категория без единого пункта в сайдбар не
показывается** — так выключаются целые разделы (см. Bluetooth ниже).
Встроенные имена (`MwCat::*` из `src/ui/features.h`):

| Имя | Экран | Фичи туда… |
|---|---|---|
| `MwCat::SubGhz` | целиком из фич (Status / Scan / Capture) | добавляются вкладкой |
| `MwCat::Wifi` | встроенный (Connect / WebUI) | фича станет следующей вкладкой |
| `"NFC"` | целиком из фич (Reader / Dump / Saved) | авто-созданная категория |
| `MwCat::Files` | встроенный экран | аналогично |
| `MwCat::Settings` | встроенный (LED/Display/Time/Wi-Fi/Theme/About + Sleep/Reboot) | аналогично |
| `MwCat::Dashboard` | удалён (Sleep/Reboot — в Settings) | — |
| `MwCat::Bluetooth` | скрыт: обе фичи выключены через `#if 0` | вернуть: `#if 0` → `#if 1` в `src/features/bluetooth_status.cpp` и `bluetooth_scan.cpp` |

Любое другое имя (`"Tools"`, `"RFID"`, …) — **новая категория, создаётся
автоматически**: секция в сайдбаре после Settings, иконка — `icon` из
регистрации. Порядок новых категорий = порядок первого упоминания.

Иконки — `namespace MwIcon` в `src/ui/features.h`: `Grid, Antenna, Wifi,
Folder, Bluetooth, Gear, Chip, Terminal, Battery, Pulse, Lock` (и
`None` — точка-заглушка). Рисуются тем же векторным стилем, что и
встроенные (`MwDraw::drawIcon`). Хочешь свою — добавь кейс в
`MwDraw::drawIcon` (это единственная правка не в своём файле, и она не
обязательна).

## 6. Рисование

Стиль интерфейса — **«Terminal-Lite»**: чёрный фон, фосфорно-зелёный
акцент, один моноширинный шрифт (Font0 5×7), без панелей и скруглений.
Фича получает спрайт-канвас `LGFX_Sprite& c` и прямоугольник контента
`(x, y, w, h)` — область между шапкой и подвалом. Оболочку (шапку,
подвал, курсор, скроллбар) рисует сам GUI. Правила:

- **не** вызывай `fillSprite` и `pushSprite` — кадр целиком делает GUI;
- **не** рисуй за пределами `(x, y, w, h)`;
- готовые виджеты в стиле GUI: `MwDraw::card(c, x, y, w, h, "ЗАГОЛОВОК")`
  (акцентный заголовок вместо панели) и `MwDraw::gauge(c, cx, cy, r,
  pct, "42%", "подпись")` — значение крупно + горизонтальная полоса
  (`pct` 0..1, `r` — полуширина полосы);
- шрифт один — `Font0`: размер текста задавай `c.setTextSize(Theme::SIZE_TEXT)`
  (мелкий, по умолчанию — GUI сбрасывает его перед `onDraw`) или
  `Theme::SIZE_MENU` (крупный, x2 — как пункты меню); текстовую метрику
  (высоты строк, отступы) бери из `Theme::ROW_LIST / ROW_MENU / PAD / KEY_COL_W`;
- цвета — только из `include/theme.h` (`Theme::*`): `BG` (чёрный),
  `ACCENT` (#39FF88), `TEXT` (значения), `TEXT_DIM` (приглушённый),
  `OK`, `WARN`, `ERR`. Всю палитру меняют в одном месте — theme.h;
  старые имена `BG_PANEL/BORDER/...` остались алиасами на чёрный;
- `c.setTextColor(fg, bg)` — второй цвет обязателен (обычно `Theme::BG`),
  иначе будет «шлейф» от старого текста;
- **кириллицы в шрифтах нет** — русский текст пиши транслитом
  (в проекте есть `translitCyrillic()`, но он приватен для Ui; для фич —
  просто латиница).

## 7. Превью stateless-фич

Если у фичи `onEnter == nullptr`, она считается stateless и её `onDraw`
вызывается **ещё до входа** — как только её пункт выбран на уровне
вкладок (и даже когда просто стоит этот раздел в сайдбаре). Так работают
мигрированные экраны SubGhz/Bluetooth: картинка видна сразу, как раньше.
Фича с `onEnter` до клика показывается карточкой-заглушкой, а `onDraw`
зовётся только после входа.

Если фича stateless, её `onDraw` не должен полагаться на то, что `onEnter`
когда-то был вызван.

## 8. Производительность и память — КРИТИЧНО

Экран — общий с SD-картой и CC1101 SPI-интерфейс, а кадр (106 КБ по SPI)
GUI рисует только когда есть зачем: пришёл ввод, раз в секунду (часы/
батарея), не доиграла анимация, или фича попросила. Это экономит и
батарею. Поэтому:

1. **Не рисуй «каждый тик».** Кадр появится сам при вводе; «живые» экраны
   (Dashboard, SubGhz, фичи) периодически перерисовываются раз в секунду,
   статичные (Wifi/Files/Settings) — раз в минуту; чтобы обновить экран
   из `onTick`, вызывай `Features::requestRedraw()` — и только когда
   реально есть что показать.
2. **`onTick()` зовётся на каждой итерации `loop()`** — это тысячи раз в
   секунду. Держи его лёгким; периодическую работу делай по таймеру:

   ```cpp
   void onTick() {
       static uint32_t last = 0;
       static float lastRssi = -128.0f;
       if (millis() - last < 250) return;   // не чаще 4 раз в секунду
       last = millis();
       float r = SubGhz::readRssiDbm();
       if (r != lastRssi) { lastRssi = r; Features::requestRedraw(); }
   }
   ```
3. **Без динамических аллокаций и тяжёлого STL** в постоянной работе
   (`std::vector`/`String` — только осознанно; heap маленький, фрагментация
   убивает Wi-Fi). Предпочитай статические буферы.
4. Никаких `delay()` в `onTick`/`onDraw` — заморозишь весь GUI. Пауза
   нужна — считай время через `millis()`.

## 9. API прошивки, доступное фичам

Заголовки подключаются относительно `src/features/` (`../hal/battery.h` и т.п.).

| Модуль | Заголовок | Что умеет |
|---|---|---|
| `Board::` | `hal/board.h` | светодиоды WS2812: `setLedColor/Mode/Brightness/Speed`, `LedMode::Off/Solid/Breathing/Tail/Rainbow`; `enterDeepSleep()`. Ввод опрашивать самому НЕ нужно |
| `SubGhz::` | `radio/subghz.h` | CC1101: `isReady()`, `setBand(MHz315/MHz433/MHz868)`, `setFrequency(mhz)`, `currentFrequencyMHz()`, `readRssiDbm()`; raw-захват: `startRawCapture(&SubGhz::sharedCapture(), ms)`, `isCapturing()`, `stopRawCapture()`, `replayRaw(capture)`, `saveRaw/loadRaw(path, capture)`; структура `RawCapture` (до 2048 фронтов) |
| `WifiMgr::` | `net/wifi_manager.h` | `startScan()/isScanning()/resultCount()/result(i)`, `connect(ssid, pass)`, `disconnect()`, `isConnected()`, `connState()`, AP-режим `startAP(preset)/stopAP()/isAPRunning()`, `releaseIdleResources()` |
| `WebUI::` | `net/webui.h` | веб-интерфейс: `start()/stop()/isRunning()`; `loop()` обслуживать не нужно |
| `Files::` | `storage/files.h` | SD/Flash-браузер: `listCount/listEntry(root, path, i)`, `FsRoot::SD/Flash`, `sdPresent()/flashPresent()` |
| `Battery::` | `hal/battery.h` | `percent()`, `voltage()`, `isCharging()`, `isPresent()` |
| `Config::` | `hal/config.h` | постоянные настройки в NVS: `getInt/setInt/getString/setString/getBool/setBool(key, …)`. Ключ ≤ 15 символов |
| `MemoryController::` | `hal/memory_controller.h` | `freeHeap()`, `minFreeHeap()`, `freePsram()`, `largestBlock()`, `fragmentationPct()` |
| `LuaEngine::` / `JsEngine::` | `scripting/*.h` | `runFile(path)` — запуск скрипта (блокирует до конца скрипта!) |
| `Nfc::` | `hal/nfc.h` | PN532 (I2C 0x24), ленивая инициализация: `ensure()/isReady()`, `readTag(Tag&)` (UID+тип), `classicReadBlock(block, data)` (перебор дефолтных ключей), `ntagReadPage(page, data)`. ISO14443A (Classic/UL/NTAG); B и FeliCa библиотека не умеет |
| `ResMgr::` | `net/resource_mgr.h` | арбитр портов: `claim/release(Resource::Http80 или Dns53, "owner")` — WebUI уходит с порта на время клейма и возвращается сам (см. ниже) |
| `ui.rawDisplay()` | `ui/ui.h` | прямой доступ к дисплею **мимо кадра GUI** — как рисуют скрипты; следующий кадр GUI всё перерисует. Для обычных фич не нужен, рисуй в `onDraw` |

**Захват :80 / :53 (captive portal, свой веб-сервер).** WebUI — не хозяин
портов. В `onEnter` фичи: `ResMgr::claim(ResMgr::Resource::Http80, "my_portal")`
и при необходимости `Dns53` — WebUI остановит свой сервер/DNS и будет
честно рапортовать `isRunning()==false`; в `onExit` — `release(...)` —
WebUI поднимется сам (если пользователь его включал). Свои серверы
поднимай после успешного `claim` (порт гарантированно свободен).
Исходящий интернет (HTTPClient, MQTT) арбитража не требует — WebUI
только слушает входящие.

**Про веб.** Веб-интерфейс — один `WebServer` на порту
80 (`src/net/webui.cpp`, маршруты регистрируются в `WebUI::start()`):
второй сервер на тот же порт не поднять, конфликт портов решается либо
другим портом (8080+), либо захватом :80 через `ResMgr`. `DNSServer` в
проекте есть: поднимается WebUI только при работающем AP (wildcard `*`
на порту 53) для captive-редиректа; фича со своим DNS клеймит `Dns53`.
AP+STA поддерживается: `WifiMgr::startAP()` сам поднимает `WIFI_AP_STA`,
если STA уже подключён.

Ничего из этого вызывать при регистрации не обязательно — фича может быть
просто картинкой.

## 10. Распиновка LilyGo T-Embed CC1101

Источник правды — `include/pins.h` (в PlatformIO выбран борд
`lilygo-t-display-s3`, потому что готового борда под T-Embed CC1101 нет;
его pins_arduino.h **другой платы — не верить**).

| GPIO | Назначение |
|---|---|
| 0 | кнопка энкодера (OK) + выход из сна |
| 3 / 38 | CC1101 GDO0 / GDO2 |
| 4 / 5 | энкодер A / B |
| 6 | боковая кнопка User (Quit) |
| 8 / 18 | I2C SDA / SCL — BQ27220 @0x55, BQ25896 @0x6B, PN532 @0x24 |
| 9 / 10 / 11 | SPI MOSI / MISO / SCK — **общая шина**: экран + CC1101 + SD |
| 12 / 13 | CC1101 CS / SD CS |
| 14 | WS2812 data (8 LED) |
| 15 | PWR_EN (питание платы) |
| 16 / 21 | экран: DC / подсветка (PWM) |
| 40 / 41 | экран: RST / CS |
| 47 / 48 | CC1101 SW1 / SW0 (антенный переключатель) |

Заняты системой: 26–32 (flash), 33–37 (OPI PSRAM). Свободные кандидаты:
1, 2, 7, 17, 19, 20, 39, 42; 43/44 (UART0 — свободны при USB-CDC); 45/46 —
страповые, осторожно. Пины 39/42/45/46 в коде не используются — перед
подключением чего-то к ним сверься со схемой платы.

## 11. Сборка и диагностика

```bash
pio run                      # сборка
pio run -t upload            # прошивка (порт COM3 из platformio.ini)
pio run -t upload --upload-port COM5
pio device monitor           # сериал 115200
pio run -t clean             # чистая пересборка
```

Проверить, что фича попала в прошивку:
`grep -a "Hello" .pio/build/t-embed-cc1101/firmware.bin` — имя должно найтись.

Если что-то не так:

| Симптом | Причина / что делать |
|---|---|
| Пункта нет в меню | файл не `.cpp`? категория `"Dashboard"`? в `REGISTER_FEATURE` передан `nullptr` вместо `onDraw`? проверь `grep -a` по `firmware.bin`; смотри сериал: реестр пишет `[MEGAWARE] feature '…' skipped (…)` |
| Пункт есть, экран пустой | рисуешь не в `(x, y, w, h)`, забыл фон (`MwDraw::card` / `fillRoundRect`), или stateless-фича рассчитывает на `onEnter`, которого не было |
| Мельтешение/лаги GUI | рисуешь каждый тик или слишком часто дёргаешь `requestRedraw()` — см. раздел 8 |
| Завис весь GUI | `delay()` в `onTick`/`onDraw` — убери |
| Числа/текст со «шлейфом» | `setTextColor(fg, bg)` без второго цвета |
| Кириллица не отображается | шрифты без кириллицы — транслит (раздел 6) |
| Устройство «выключилось» | ты держал Quit 3 с — это сон; будит кнопка энкодера |
| Прошивка не льётся | занят COM-порт (закрой монитор), или порт другой — `--upload-port` |

Лимиты реестра (константы в `src/ui/features.h`, при необходимости подними):
`MW_MAX_FEATURES = 24` фич всего, `MW_MAX_NEW_CATEGORIES = 6` новых
категорий. Реестр лежит в статической памяти (~28 Б на фичу), heap не тратит.

## 12. Известные ограничения

- `Dashboard`, `Wifi`, `Files`, `Settings` — встроенные экраны оболочки
  (их состояние живёт внутри класса `Ui`): фичи добавляются туда только
  как дополнительные вкладки, свои экраны внутрь них не встроить.
- Экранная клавиатура (ввод Wi-Fi пароля) — только для встроенного Wi-Fi;
  фичам текстовый ввод недоступен.
- `onTick`/`onDraw` выполняются в главном цикле — блокируешь ты блокируешь
  всё GUI (веб-интерфейс при этом продолжает отвечать, но экран нет).
- `include/tft_setup.h` — legacy для библиотеки TFT_eSPI, в сборке не
  участвует, не используй.
