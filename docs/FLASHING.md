# Как собирать и заливать прошивку (PlatformIO CLI)

`pio` лежит **не в PATH**, а в окружении PlatformIO:
```
C:\Users\super\.platformio\penv\Scripts\pio.exe
```
(в Git Bash: `~/.platformio/penv/Scripts/pio.exe`)

## Быстро: добавить pio в PATH (один раз)

**PowerShell:**
```powershell
setx PATH "$env:PATH;C:\Users\super\.platformio\penv\Scripts"
```
**cmd:**
```cmd
setx PATH "%PATH%;C:\Users\super\.platformio\penv\Scripts"
```
После `setx` перезапусти терминал. В текущей сессии можно работать полным путём.

## Команды (из папки проекта `C:\PlatformIO\megaware`)

| Что | Команда |
|---|---|
| Собрать | `pio run` |
| Собрать и залить | `pio run -t upload` |
| Залить на другой порт | `pio run -t upload --upload-port COM5` |
| Сериал-монитор (115200) | `pio device monitor` |
| Чистая пересборка | `pio run -t clean` |
| Только размер | `pio run` (смотреть RAM/Flash в конце) |

В VSCode с расширением PlatformIO всё то же самое кнопками внизу панели
(→/⏏ = upload, розетка = monitor).

## Если «Could not open COM3 / порт не существует»

1. Плату могли переименовать другие USB-устройства: `pio device list` —
   смотри актуальный COM.
2. Прошивка зависла в странном USB-режиме: **зажми кнопку энкодера
   (это BOOT, GPIO0), коротко нажми reset, отпусти** — плата войдёт в
   download-режим, появится ROM-порт; заливка пройдёт с ним. После
   загрузки новой прошивки порт вернётся к обычному.
3. Порт занят монитором — закрой `pio device monitor`.

## Альтернатива кабелю: OTA

WebUI → **UPDATE** → выбрать
`.pio/build/t-embed-cc1101/firmware.bin` → устройство зальёт и
перезагрузится само. Работает и по Wi-Fi, и по кабелю/W5500.

## Сборка «молчит» долго — это нормально

Полная пересборка (после смены флагов/зависимостей) занимает 3–6 минут;
инкрементальная — 40–90 с. Если вывод пайпится через `grep`, строки
придут только в конце. Смотри лог целиком: `pio run 2>&1 | tee build.log`.
