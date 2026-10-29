# MEGAWARE

Firmware for the **LilyGo T-Embed CC1101** (ESP32-S3, 16 MB flash, 8 MB PSRAM). A multi-tool for every radio on this board: sub-GHz (CC1101), NFC (PN532), infrared, BLE, 2.4 GHz (RF24), Ethernet (W5500) — plus Wi-Fi with a built-in web interface, file manager, on-device scripting and firmware updating.

**Tested only on LilyGo T-Embed CC1101.** Other boards listed below are build-configured but untested.

## Features

| Category | What it does |
|---|---|
| **Wi-Fi** | Connect (scan / WPA / captive DNS), WebUI server, WiFi analyzer, host scanner (TCP /24), HTTP client (presets), mDNS browser, Captive Portal (your page), AP Spam (beacon flood) |
| **Sub-GHz** | CC1101 status, live RSSI, raw capture / replay / save (.sub format, Flipper-compatible), spectrum analyzer (RF + WebUI), signal library, decoder |
| **NFC** | PN532 reader (UID / type / ATS), MIFARE Classic dump (default keys + dictionary), Saved dumps (hex editor / write-back / export .nfc .mfd / key map), NDEF write (URL), NTAG password tools, emulation (UID phase) |
| **Bluetooth** | BLE scan (RSSI sorted), AirTag / Find My tracker detector, BLE Typer (HID keyboard) |
| **Ethernet** | W5500 via Grove port (DHCP, WebUI over cable) |
| **RF24** | 2.4 GHz scanner (external module in Grove port) |
| **Files** | SD / LittleFS browser, run Lua / JS scripts, MP3 / WAV playback via speaker, flash a `firmware.bin` right from the file list (hold OK) |
| **Settings** | LED (color / mode / speed / click blip / rotate glow), Display (brightness / auto-dim / rotation), Theme (accent presets), Time (UTC offset, saved), About, Sleep, Reboot |
| **WebUI** | File manager (drag & drop, editor, hex editor), Terminal (live status, command console with `/docs` reference, spectrum display), APDU console, OTA firmware update |
| **Scripting** | Lua + JS: `gfx`, `input`, `led`, `subghz`, `nfc`, `wifi`, `battery`, `config`, `fs`, `sys` |
| **Games** | Snake (Lua + JS examples in `games/`) |

## Navigation

| Action | Result |
|---|---|
| Rotate encoder | Move selection |
| Click encoder (OK) | Enter / confirm / toggle |
| Short click side button (Quit) | Back one level |
| Long click encoder | Context action (folder up, capture stop, etc.) |
| Hold Quit 3 s | Deep sleep (wake: encoder button) |

## Quick start

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
2. Clone this repo, open folder in PlatformIO
3. `pio run -t upload` — build and flash via USB
4. `pio device monitor` — serial console (115200 baud)

If `pio` is not in PATH, use the full path:
```
%USERPROFILE%\.platformio\penv\Scripts\pio.exe
```
See [docs/FLASHING.md](docs/FLASHING.md) for details.

## WebUI

Open `http://megaware.local/` or the device IP in a browser (same network or AP):

- **Files** — SD / Flash manager: browse, create, edit, delete, drag & drop upload, hex editor (≤ 16 KB), download
- **System Info** — live hardware status
- **Terminal** — real-time status, command console (type `help` for the full list), spectrum display, PN532 APDU console, [command reference](http://megaware.local/docs)
- **About** — full hardware info (matches device screen)
- **OTA UPDATE** — firmware update from the browser over Wi-Fi (not USB)

## Adding features

New feature = **one file** in `src/features/`. See [docs/ADDING_FEATURES.md](docs/ADDING_FEATURES.md) and the template `src/features/_template.cpp.example`.

```cpp
REGISTER_FEATURE("My Feature", "Tools", MwIcon::Chip, 100,
                 onEnter, onInput, onTick, onDraw, onExit);
```

## Scripting

Lua and JS with the same API: `gfx`, `input`, `led`, `subghz` (+ `transmitFile`, `transmit`), `nfc`, `wifi`, `battery`, `config`, `fs`, `sys`. Examples in `games/`.

## Board support

| Board | Status | Notes |
|---|---|---|
| LilyGo T-Embed CC1101 | ✅ tested | Primary target |
| LilyGo T-Embed (no CC1101) | 🔧 build-configured | Same pins, no CC1101 |
| LilyGo T-Display S3 | 🔧 build-configured | No CC1101, no PN532, no speaker |
| M5Stack Cardputer | 📋 planned | Keyboard input (needs input layer) |
| M5StickC Plus 2 | 📋 planned | Button input (needs input layer) |
| LILYGO T-Deck | 📋 planned | Keyboard + trackball |
| LILYGO T-Watch S3 | 📋 planned | Touchscreen input |

Boards without encoders need an input abstraction layer (mapping keyboard / buttons / touch to the same rotate / click / hold events). The `InputState` struct is already hardware-agnostic — the gap is the driver.

## Project structure

```
include/pins.h          — pin map (T-Embed CC1101, verified from Bruce + LilyGo)
include/theme.h         — theme palette (all colors in one place)
src/main.cpp            — init + main loop
src/hal/                — hardware drivers (board, battery, nfc, ir, blescan, audio_out...)
src/radio/subghz.*      — CC1101 driver
src/net/                — WebUI, Wi-Fi manager, resource arbiter, captive portal
src/storage/files.*     — SD / LittleFS browser
src/ui/                 — feature registry, theme, device screens
src/features/           — feature plugins (one file = one feature)
src/scripting/          — Lua + JS engines
games/                  — example scripts (Snake in Lua + JS)
docs/                   — FLASHING.md, ADDING_FEATURES.md
```

## Known limitations

- Cyrillic is transliterated (fonts have no Cyrillic glyphs)
- MIFARE Classic / NTAG are handled via PN532 native commands, not APDU (APDU console is for ISO14443-4 / DESFire)
- `include/tft_setup.h` is legacy (unused, TFT_eSPI not in build)

## License

ELk (JS engine) is AGPLv3 — see README section in the source repo. Everything else: MIT.
