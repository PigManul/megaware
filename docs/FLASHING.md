# How to Build and Flash the Firmware (PlatformIO CLI)

`pio` is **not in PATH**; it lives in the PlatformIO environment:
```
%USERPROFILE%\.platformio\penv\Scripts\pio.exe
```
(in Git Bash: `~/.platformio/penv/Scripts/pio.exe`)

## Quick: add pio to PATH (one time)

**PowerShell:**
```powershell
setx PATH "$env:PATH;$env:USERPROFILE\.platformio\penv\Scripts"
```
**cmd:**
```cmd
setx PATH "%PATH%;%USERPROFILE%\.platformio\penv\Scripts"
```
After `setx`, restart the terminal. In the current session you can work with the full path.

## Commands (from the project folder)

| What | Command |
|---|---|
| Build | `pio run` |
| Build and flash | `pio run -t upload` |
| Flash to a different port | `pio run -t upload --upload-port COM5` |
| Serial monitor (115200) | `pio device monitor` |
| Clean rebuild | `pio run -t clean` |
| Size only | `pio run` (check RAM/Flash at the end) |

The upload port is auto-detected; `upload_port` / `monitor_port` in
`platformio.ini` are intentionally unset — override per-invocation with
`--upload-port COM5` if the board is not the only serial device.

In VSCode with the PlatformIO extension, everything is the same with the buttons on the bottom panel
(→/⏏ = upload, socket = monitor).

## If "Could not open COM port / port does not exist"

1. The board may have been renamed by other USB devices: `pio device list` —
   check the current COM.
2. The firmware is stuck in a strange USB mode: **hold the encoder button
   (this is BOOT, GPIO0), briefly press reset, release** — the board will enter
   download mode, a ROM port will appear; flashing will proceed with it. After
   loading the new firmware, the port returns to normal.
3. The port is busy with the monitor — close `pio device monitor`.

## Other ways to flash (no PlatformIO needed)

- **OTA over Wi-Fi** — WebUI → **OTA UPDATE** → select
  `.pio/build/t-embed-cc1101/firmware.bin` → the device flashes the second
  slot and reboots itself. File must fit 4.5 MB. Works over Wi-Fi and
  cable/W5500.
- **On-device from file** — copy `firmware.bin` to the SD card or flash
  storage, on the device open *Files* and **hold OK** on the file.

## The build is "silent" for a long time — this is normal

A full rebuild (after changing flags/dependencies) takes 3–6 minutes;
incremental — 40–90 s. If the output is piped through `grep`, lines
will only arrive at the end. Look at the log in full: `pio run 2>&1 | tee build.log`.
