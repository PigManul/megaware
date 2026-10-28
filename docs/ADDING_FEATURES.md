# MEGAWARE: How to Add Your Own Feature to the GUI

This document is self-contained: it is written for someone who has never seen the project before. All you need is an installed [PlatformIO](https://platformio.org/) (VS Code extension or CLI) and this project's folder.

---

## 1. What This Project Is, in a Nutshell

MEGAWARE is firmware for the **LilyGo T-Embed CC1101** (ESP32-S3, 16 MB flash, 8 MB PSRAM): ST7789 170×320 display, CC1101 radio module (Sub-GHz), encoder with button, side Quit button, 8× WS2812 LEDs, BQ27220 battery gauge, microSD slot.

The GUI looks like this: a **sidebar** on the left with section icons (Dashboard, SubGhz, Wifi, Files, Bluetooth, Settings, …) → sub-tabs on top (e.g., for SubGhz: Status / Scan / Capture) → content in a card on the right. Controls:

| Action | Result |
|---|---|
| rotate encoder | scrolls the current level (sections / tabs / within a feature) |
| click encoder (OK) | enter / action |
| short click on side button (**Quit**) | one level up; **from a feature — exit** |
| long click on encoder (LongPress) | context action (e.g., folder up in Files) |
| hold Quit for 3 seconds | power off (deep sleep); wakes on encoder button |

## 2. The Core Idea

**One feature = one file** in `src/features/`. In this file, the feature declares everything itself:

- the menu item name and icon;
- the GUI category to place it in (a new category is created automatically);
- the order within the category;
- the code: enter, input handling, loop, draw, exit.

No more **edits anywhere else** in the project: not `main.cpp`, not the GUI, not menu lists, not `platformio.ini`. All feature files are collected by the registry (`src/ui/features.h` / `features.cpp`) — at firmware startup it builds menu items from registered features. Delete the file — the item disappears.

Ready-made examples in the project: `src/features/subghz_*.cpp`, `src/features/bluetooth_*.cpp` (these are the real SubGhz/Bluetooth screens, migrated into the registry). A template with detailed comments: `src/features/_template.cpp.example`.

## 3. Step by Step: A Minimal Feature from Scratch to Hardware

**Step 1.** Create the file `src/features/hello.cpp` (any name, extension must be `.cpp`):

```cpp
// ============================================================
//  Example: a "Hello" item in a new "Tools" category.
//  Rotation changes the counter, click resets, Quit exits.
// ============================================================
#include "../ui/features.h"          // registry, icons, drawing helpers
#include "../../include/theme.h"     // Theme::* palette

namespace {
    int counter = 0;                 // feature state — ordinary static variables

    // Input handling. Called ONLY on a real event.
    // Return true if the event is handled; false — "not mine".
    bool onInput(const InputState& in) {
        if (in.rotationSteps != 0) {          // rotation: +N/-N steps
            counter += in.rotationSteps;
            Features::requestRedraw();        // request a frame redraw
            return true;
        }
        if (in.event == InputEvent::Click) {  // encoder click
            counter = 0;
            Features::requestRedraw();
            return true;
        }
        return false; // don't consume Quit — the GUI will exit the feature itself
    }

    // Drawing. (x, y, w, h) — the content rectangle, draw only within it.
    void onDraw(LGFX_Sprite& c, int x, int y, int w, int h) {
        MwDraw::card(c, x, y, w, h, "HELLO");  // card in GUI style
        c.setFont(&fonts::Font4);              // large font
        c.setTextDatum(textdatum_t::middle_center);
        c.setTextColor(Theme::TEXT, Theme::BG_PANEL);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", counter);
        c.drawString(buf, x + w / 2, y + h / 2);

        c.setFont(&fonts::Font0);              // small font
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG_PANEL);
        c.drawString("rotate: +/-  click: reset", x + 10, y + h - 14);
    }
}

// REGISTRATION — the only mandatory line.
// REGISTER_FEATURE(name, category, icon, order,
//                  onEnter, onInput, onTick, onDraw, onExit);
// Unused handlers — nullptr. At minimum onDraw is required.
REGISTER_FEATURE("Hello", "Tools", MwIcon::Terminal, 10,
                 nullptr, onInput, nullptr, onDraw, nullptr);
```

**Step 2.** Build and flash:

```bash
pio run -t upload        # build + flash over COM3 (port set in platformio.ini)
pio device monitor       # serial log, 115200 (Ctrl+C — exit)
```

If the board is on a different port: `pio run -t upload --upload-port COM5`.

**Step 3.** On the device: a **Tools** section with a terminal icon will appear at the bottom of the sidebar (the category created itself because `"Tools"` is a new name). Inside — the **Hello** item. Click — enter, rotation changes the number, click — reset, Quit — exit.

That's it. Nothing else in the project needs to be changed.

## 4. REGISTER_FEATURE: Full Contract

```cpp
REGISTER_FEATURE(name, category, icon, order,
                 onEnter, onInput, onTick, onDraw, onExit);
```

| Argument | Type | Required | Meaning |
|---|---|---|---|
| `name` | `const char*` | yes | item name (fits in the tab button, small font) |
| `category` | `const char*` | yes | see section 5 |
| `icon` | `MwIcon::Id` | yes | icon of the **new** category; for an existing one — `MwIcon::None` |
| `order` | `int8_t` | yes | order within the category: lower — earlier; equal — depends on layout, so set it explicitly (0, 10, 20…) |
| `onEnter` | `void ()` | no | entering the feature (click on the item). If `nullptr` — the feature is stateless, see section 7 |
| `onInput` | `bool (const InputState&)` | no | input; return `true` = event consumed |
| `onTick` | `void ()` | no | loop/update — **every tick of `loop()`**, see section 8 |
| `onDraw` | `void (LGFX_Sprite&, int x, int y, int w, int h)` | **yes** | content drawing |
| `onExit` | `void ()` | no | leaving the feature — return hardware to its initial state |

All signatures are in `struct Feature` in `src/ui/features.h`.

### Input (`InputState`, `src/hal/board.h`)

- `in.rotationSteps` — how many encoder clicks it was turned (+/-).
- `in.event` — `InputEvent::Click` (short OK click), `LongPress` (OK hold), `UserKey` (short Quit click), `None`.
- Holding Quit (3 s) **does not reach** the feature — that's power-off, the system handles it itself. Don't react to `in.powerHeldMs`.

**Main exit rule:** if `onInput` returns `false` for `InputEvent::UserKey`, the GUI will exit the feature itself to the tab level and call `onExit()`. So to exit, it's enough to **not consume** Quit — you don't need to write your own exit code. `LongPress` without handling is simply ignored.

**Don't call `Board::pollInput()` yourself** — input already arrives in `onInput`; an extra poll would steal events from the rest of the GUI.

### State

Feature state is ordinary `static` variables inside the anonymous namespace of your file. `onEnter` is needed when you must initialize/disable something on entry, and `onExit` — to restore it (e.g., stop CC1101 reception).

## 5. Categories

A category = a sidebar section. **A category with no items is not shown in the sidebar** — this is how entire sections are disabled (see Bluetooth below). Built-in names (`MwCat::*` from `src/ui/features.h`):

| Name | Screen | Features go there… |
|---|---|---|
| `MwCat::SubGhz` | entirely from features (Status / Scan / Capture) | added as a tab |
| `MwCat::Wifi` | built-in (Connect / WebUI) | feature becomes the next tab |
| `"NFC"` | entirely from features (Reader / Dump / Saved) | auto-created category |
| `MwCat::Files` | built-in screen | likewise |
| `MwCat::Settings` | built-in (LED/Display/Time/Wi-Fi/Theme/About + Sleep/Reboot) | likewise |
| `MwCat::Dashboard` | removed (Sleep/Reboot — in Settings) | — |
| `MwCat::Bluetooth` | hidden: both features are disabled via `#if 0` | restore: `#if 0` → `#if 1` in `src/features/bluetooth_status.cpp` and `bluetooth_scan.cpp` |

Any other name (`"Tools"`, `"RFID"`, …) — **a new category, created automatically**: a section in the sidebar after Settings, icon — `icon` from the registration. The order of new categories = order of first mention.

Icons — `namespace MwIcon` in `src/ui/features.h`: `Grid, Antenna, Wifi, Folder, Bluetooth, Gear, Chip, Terminal, Battery, Pulse, Lock` (and `None` — placeholder dot). They are drawn in the same vector style as the built-in ones (`MwDraw::drawIcon`). Want your own — add a case to `MwDraw::drawIcon` (this is the only edit outside your own file, and it's optional).

## 6. Drawing

The interface style is **"Terminal-Lite"**: black background, phosphor-green accent, a single monospaced font (Font0 5×7), no panels or rounded corners. The feature receives a sprite canvas `LGFX_Sprite& c` and the content rectangle `(x, y, w, h)` — the area between header and footer. The shell (header, footer, cursor, scrollbar) is drawn by the GUI itself. Rules:

- **do not** call `fillSprite` or `pushSprite` — the GUI makes the whole frame;
- **do not** draw outside `(x, y, w, h)`;
- ready-made widgets in GUI style: `MwDraw::card(c, x, y, w, h, "TITLE")` (accent title instead of a panel) and `MwDraw::gauge(c, cx, cy, r, pct, "42%", "caption")` — value large + horizontal bar (`pct` 0..1, `r` — half-width of the bar);
- one font — `Font0`: set text size with `c.setTextSize(Theme::SIZE_TEXT)` (small, default — the GUI resets it before `onDraw`) or `Theme::SIZE_MENU` (large, x2 — like menu items); take text metrics (line heights, paddings) from `Theme::ROW_LIST / ROW_MENU / PAD / KEY_COL_W`;
- colors — only from `include/theme.h` (`Theme::*`): `BG` (black), `ACCENT` (#39FF88), `TEXT` (values), `TEXT_DIM` (muted), `OK`, `WARN`, `ERR`. The whole palette is changed in one place — theme.h; old names `BG_PANEL/BORDER/...` remain aliases to black;
- `c.setTextColor(fg, bg)` — the second color is mandatory (usually `Theme::BG`), otherwise there will be a "trail" from the old text;
- **there is no Cyrillic in the fonts** — write Russian text in transliteration (the project has `translitCyrillic()`, but it's private to Ui; for features — just Latin).

## 7. Preview of Stateless Features

If a feature has `onEnter == nullptr`, it's considered stateless and its `onDraw` is called **before entering** — as soon as its item is selected at the tab level (and even when this section is just sitting in the sidebar). This is how the migrated SubGhz/Bluetooth screens work: the picture is visible immediately, as before. A feature with `onEnter` shows a placeholder card until the click, and `onDraw` is called only after entering.

If a feature is stateless, its `onDraw` must not rely on `onEnter` ever having been called.

## 8. Performance and Memory — CRITICAL

The display shares the SPI bus with the SD card and CC1101, and the GUI only draws a frame (106 KB over SPI) when there's a reason: input arrived, once per second (clock/battery), an animation hasn't finished, or the feature requested it. This saves both power and battery. Therefore:

1. **Don't draw "every tick".** A frame will appear on its own on input; "live" screens (Dashboard, SubGhz, features) are redrawn periodically once per second, static ones (Wifi/Files/Settings) — once per minute; to update the screen from `onTick`, call `Features::requestRedraw()` — and only when there's actually something to show.
2. **`onTick()` is called on every iteration of `loop()`** — thousands of times per second. Keep it light; do periodic work on a timer:

   ```cpp
   void onTick() {
       static uint32_t last = 0;
       static float lastRssi = -128.0f;
       if (millis() - last < 250) return;   // no more than 4 times per second
       last = millis();
       float r = SubGhz::readRssiDbm();
       if (r != lastRssi) { lastRssi = r; Features::requestRedraw(); }
   }
   ```
3. **No dynamic allocations or heavy STL** in continuous operation (`std::vector`/`String` — only deliberately; the heap is small, fragmentation kills Wi-Fi). Prefer static buffers.
4. No `delay()` in `onTick`/`onDraw` — you'll freeze the whole GUI. Need a pause — count time via `millis()`.

## 9. Firmware API Available to Features

Headers are included relative to `src/features/` (`../hal/battery.h`, etc.).

| Module | Header | What it can do |
|---|---|---|
| `Board::` | `hal/board.h` | WS2812 LEDs: `setLedColor/Mode/Brightness/Speed`, `LedMode::Off/Solid/Breathing/Tail/Rainbow`; `enterDeepSleep()`. You do NOT need to poll input yourself |
| `SubGhz::` | `radio/subghz.h` | CC1101: `isReady()`, `setBand(MHz315/MHz433/MHz868)`, `setFrequency(mhz)`, `currentFrequencyMHz()`, `readRssiDbm()`; raw capture: `startRawCapture(&SubGhz::sharedCapture(), ms)`, `isCapturing()`, `stopRawCapture()`, `replayRaw(capture)`, `saveRaw/loadRaw(path, capture)`; `RawCapture` struct (up to 2048 edges) |
| `WifiMgr::` | `net/wifi_manager.h` | `startScan()/isScanning()/resultCount()/result(i)`, `connect(ssid, pass)`, `disconnect()`, `isConnected()`, `connState()`, AP mode `startAP(preset)/stopAP()/isAPRunning()`, `releaseIdleResources()` |
| `WebUI::` | `net/webui.h` | web interface: `start()/stop()/isRunning()`; no need to service `loop()` |
| `Files::` | `storage/files.h` | SD/Flash browser: `listCount/listEntry(root, path, i)`, `FsRoot::SD/Flash`, `sdPresent()/flashPresent()` |
| `Battery::` | `hal/battery.h` | `percent()`, `voltage()`, `isCharging()`, `isPresent()` |
| `Config::` | `hal/config.h` | persistent settings in NVS: `getInt/setInt/getString/setString/getBool/setBool(key, …)`. Key ≤ 15 characters |
| `MemoryController::` | `hal/memory_controller.h` | `freeHeap()`, `minFreeHeap()`, `freePsram()`, `largestBlock()`, `fragmentationPct()` |
| `LuaEngine::` / `JsEngine::` | `scripting/*.h` | `runFile(path)` — run a script (blocks until the script ends!) |
| `Nfc::` | `hal/nfc.h` | PN532 (I2C 0x24), lazy initialization: `ensure()/isReady()`, `readTag(Tag&)` (UID+type), `classicReadBlock(block, data)` (iterates default keys), `ntagReadPage(page, data)`. ISO14443A (Classic/UL/NTAG); B and FeliCa are not supported by the library |
| `ResMgr::` | `net/resource_mgr.h` | port arbiter: `claim/release(Resource::Http80 or Dns53, "owner")` — WebUI leaves the port during the claim and returns on its own (see below) |
| `ui.rawDisplay()` | `ui/ui.h` | direct display access **bypassing the GUI frame** — how scripts draw; the next GUI frame will redraw everything. Not needed for normal features, draw in `onDraw` |

**Claiming :80 / :53 (captive portal, custom web server).** WebUI is not the port owner. In the feature's `onEnter`: `ResMgr::claim(ResMgr::Resource::Http80, "my_portal")` and, if needed, `Dns53` — WebUI will stop its server/DNS and will honestly report `isRunning()==false`; in `onExit` — `release(...)` — WebUI will come back up itself (if the user had it enabled). Start your own servers after a successful `claim` (the port is guaranteed free). Outgoing internet (HTTPClient, MQTT) requires no arbitration — WebUI only listens for incoming connections.

**About the web.** The web interface is a single `WebServer` on port 80 (`src/net/webui.cpp`, routes are registered in `WebUI::start()`): a second server on the same port can't be started, port conflicts are resolved either with a different port (8080+) or by claiming :80 via `ResMgr`. `DNSServer` exists in the project: WebUI starts it only when AP is running (wildcard `*` on port 53) for captive redirect; a feature with its own DNS claims `Dns53`. AP+STA is supported: `WifiMgr::startAP()` itself brings up `WIFI_AP_STA` if STA is already connected.

None of this has to be called at registration — a feature can simply be a picture.

## 10. LilyGo T-Embed CC1101 Pinout

Source of truth — `include/pins.h` (in PlatformIO the board `lilygo-t-display-s3` is selected because there's no ready-made board for T-Embed CC1101; its pins_arduino.h is **from a different board — don't trust it**).

| GPIO | Purpose |
|---|---|
| 0 | encoder button (OK) + wake from sleep |
| 3 / 38 | CC1101 GDO0 / GDO2 |
| 4 / 5 | encoder A / B |
| 6 | side User button (Quit) |
| 8 / 18 | I2C SDA / SCL — BQ27220 @0x55, BQ25896 @0x6B, PN532 @0x24 |
| 9 / 10 / 11 | SPI MOSI / MISO / SCK — **shared bus**: display + CC1101 + SD |
| 12 / 13 | CC1101 CS / SD CS |
| 14 | WS2812 data (8 LEDs) |
| 15 | PWR_EN (board power) |
| 16 / 21 | display: DC / backlight (PWM) |
| 40 / 41 | display: RST / CS |
| 47 / 48 | CC1101 SW1 / SW0 (antenna switch) |

Used by the system: 26–32 (flash), 33–37 (OPI PSRAM). Free candidates: 1, 2, 7, 17, 19, 20, 39, 42; 43/44 (UART0 — free with USB-CDC); 45/46 — strapping pins, be careful. Pins 39/42/45/46 are not used in the code — before connecting anything to them, check against the board schematic.

## 11. Building and Diagnostics

```bash
pio run                      # build
pio run -t upload            # flash (COM3 from platformio.ini)
pio run -t upload --upload-port COM5
pio device monitor           # serial 115200
pio run -t clean             # clean rebuild
```

To verify the feature made it into the firmware: `grep -a "Hello" .pio/build/t-embed-cc1101/firmware.bin` — the name should be found.

If something is wrong:

| Symptom | Cause / what to do |
|---|---|
| Item not in the menu | file not `.cpp`? category `"Dashboard"`? `nullptr` passed instead of `onDraw` in `REGISTER_FEATURE`? check with `grep -a` on `firmware.bin`; check the serial: the registry prints `[MEGAWARE] feature '…' skipped (…)` |
| Item present, screen empty | drawing outside `(x, y, w, h)`, forgot the background (`MwDraw::card` / `fillRoundRect`), or a stateless feature relies on `onEnter` that never happened |
| GUI flicker/lag | drawing every tick or calling `requestRedraw()` too often — see section 8 |
| Entire GUI frozen | `delay()` in `onTick`/`onDraw` — remove it |
| Numbers/text with a "trail" | `setTextColor(fg, bg)` without the second color |
| Cyrillic doesn't display | fonts have no Cyrillic — transliterate (section 6) |
| Device "powered off" | you held Quit for 3 s — that's sleep; wakes on encoder button |
| Firmware won't flash | COM port busy (close the monitor), or wrong port — `--upload-port` |

Registry limits (constants in `src/ui/features.h`, raise if needed): `MW_MAX_FEATURES = 24` features total, `MW_MAX_NEW_CATEGORIES = 6` new categories. The registry lives in static memory (~28 B per feature), doesn't use heap.

## 12. Known Limitations

- `Dashboard`, `Wifi`, `Files`, `Settings` — built-in shell screens (their state lives inside the `Ui` class): features can only be added there as extra tabs; custom screens cannot be embedded inside them.
- The on-screen keyboard (Wi-Fi password entry) is only for the built-in Wi-Fi; text input is unavailable to features.
- `onTick`/`onDraw` run in the main loop — if you block, you block the entire GUI (the web interface keeps responding, but the screen doesn't).
- `include/tft_setup.h` — legacy for the TFT_eSPI library, not part of the build, don't use it.