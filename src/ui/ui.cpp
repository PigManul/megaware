#include "ui.h"
#include "../../include/theme.h"
#include "../../include/pins.h"
#include "../net/webui.h"
#include "../hal/config.h"
#include "../hal/nfc.h"
#include "../radio/subghz.h"
#include <esp_system.h>
#include <time.h>
#include <ctype.h>
#include <Wire.h>
#include <SD.h>
#include <LittleFS.h>
#include "../hal/audio_out.h"
#include "../hal/ir.h"
#include "../net/portal.h"
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp32s3/rom/miniz.h> // ROM tinfl: PDF FlateDecode streams

// MP3: single-header decoder (public domain), implementation included once.
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <minimp3.h>
#pragma GCC diagnostic pop

Ui ui;

namespace {
    constexpr int Y_OFFSET = 0;

    namespace kb {
        const char* lettersRows[3][10] = {
            {"q","w","e","r","t","y","u","i","o","p"},
            {"a","s","d","f","g","h","j","k","l",""},
            {"^","z","x","c","v","b","n","m","",""},
        };
        const int lettersRowLen[3] = {10, 9, 8};
        // 6 keys — DEL and TXT live here; the drawing loop size must
        // match the array length, otherwise we read garbage out of bounds
        const char* funcRowLetters[6] = {"DEL", "TXT", "123", "SPACE", "OK", "ESC"};

        const char* symsRows[3][10] = {
            {"1","2","3","4","5","6","7","8","9","0"},
            {"-","_",".","@","!","#","$","%","&","*"},
            {"(",")","+","=",":",";","?","/","",""},
        };
        const int symsRowLen[3] = {10, 10, 8};
        const char* funcRowSyms[6] = {"DEL", "TXT", "ABC", "SPACE", "OK", "ESC"};
    }

    bool i2cProbe(uint8_t addr) {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }

    // List scroll window: the cursor stays visible; on a screen change
    // (id) the window resets to the top. One static slot: screens
    // are drawn one per frame.
    int scrollWindow(int id, int selected, int visible, int total) {
        static int lastId = -1;
        static int top = 0;
        if (id != lastId) { lastId = id; top = 0; }
        if (selected < top) top = selected;
        if (selected >= top + visible) top = selected - visible + 1;
        if (visible > 0 && top > total - visible) top = total - visible;
        if (top < 0) top = 0;
        return top;
    }

    // Transliteration straight into a buffer: hot drawing paths (the network
    // list is drawn on every frame) must not allocate String — it
    // fragments the internal heap and hastens "RAM overflow".
    void translitToBuf(const String& in, char* out, size_t n) {
        static const char* const table[64] = {
            "A","B","V","G","D","E","ZH","Z","I","Y","K","L","M","N","O","P",
            "R","S","T","U","F","H","TS","CH","SH","SCH","","Y","","E","YU","YA",
            "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
            "r","s","t","u","f","h","ts","ch","sh","sch","","y","","e","yu","ya",
        };
        size_t o = 0, i = 0;
        if (n == 0) return;
        while (i < in.length() && o + 1 < n) {
            uint8_t c0 = (uint8_t)in[i];
            if (c0 < 0x80) { out[o++] = (char)c0; i++; continue; }
            if ((c0 & 0xE0) == 0xC0 && i + 1 < in.length()) {
                uint8_t c1 = (uint8_t)in[i + 1];
                uint16_t cp = (uint16_t)(((c0 & 0x1F) << 6) | (c1 & 0x3F));
                i += 2;
                if (cp == 0x0401) { out[o++]='Y'; out[o++]='O'; }
                else if (cp == 0x0451) { out[o++]='y'; out[o++]='o'; }
                else if (cp >= 0x0410 && cp <= 0x044F) {
                    const char* t = table[cp - 0x0410];
                    while (*t && o + 1 < n) out[o++] = *t++;
                    if (o + 1 >= n) break; // buffer ran out — do not lose position i
                }
                else out[o++] = '?';
                continue;
            }
            if ((c0 & 0xF0) == 0xE0) { out[o++] = '?'; i += 3; continue; }
            if ((c0 & 0xF8) == 0xF0) { out[o++] = '?'; i += 4; continue; }
            i++;
        }
        if (o >= n) o = n - 1; // two-char writes (YO/yo) can reach n — clamp before the NUL
        out[o] = 0;
    }

    // (translitCyrillic helper removed — the keyboard title now
    // transliterates straight into a buffer, no String per redraw)

    // List/settings row: "> label ..... value" (value on the right).
    void drawListRow(LGFX_Sprite& c, int x, int y, int w, bool selected, bool editing,
                     const char* label, const char* value, uint16_t valueCol) {
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::middle_left);
        if (selected) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(">", x, y);
        }
        c.setTextColor(selected ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
        c.drawString(label, x + 12, y);
        if (value && value[0]) {
            c.setTextDatum(textdatum_t::middle_right);
            c.setTextColor(editing ? Theme::ACCENT : valueCol, Theme::BG);
            c.drawString(value, x + w - 4, y);
            c.setTextDatum(textdatum_t::middle_left);
        }
    }

    // ---- Player (MP3 via minimp3, WAV — own header parser) ----
    constexpr int MP3_BUF = 2048;   // >= max frame size (~1044 B)
    mp3dec_t mp3dec;
    uint8_t mp3buf[MP3_BUF];
    int mp3Fill = 0;
    bool playIsMp3 = false;
    bool playActive = false;        // mirror of Ui::playing for this namespace
    File playFile;
    char playTitle[40] = "";
    uint32_t playPos = 0, playTotal = 0;
    uint8_t playChannels = 1;

    void refillMp3() {
        if (!playFile || !playFile.available()) return;
        int free_ = MP3_BUF - mp3Fill;
        if (free_ <= 0) return;
        int n = playFile.read(mp3buf + mp3Fill, free_);
        if (n > 0) { mp3Fill += n; playPos += n; }
    }

    void netCheckTick(); // implemented in the second ns (near drawAboutScreen)
} // namespace

// =====================================================================
//  Player: Mp3/WAV from Files (hold on a file). Feeds I2S on each tick.
// =====================================================================
void Ui::startPlayback(const String& path) {
    stopPlayback();
    fs::FS& playFs = Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS;
    File f = playFs.open(path, FILE_READ);
    if (!f) return;

    bool isMp3 = path.endsWith(".mp3") || path.endsWith(".MP3");
    if (isMp3) {
        mp3dec_init(&mp3dec);
        mp3Fill = 0;
        playChannels = 2; // refined from the frames
        AudioOut::begin(44100);
        playTotal = f.size();
        playPos = 0;
        playIsMp3 = true;
    } else {
        // WAV: RIFF/WAVE, look for fmt + data (16-bit PCM)
        uint8_t hdr[12];
        if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { f.close(); return; }
        uint16_t ch = 1, bits = 16;
        uint32_t rate = 44100, dataOff = 0, dataSize = 0;
        bool found = false;
        while (f.available()) {
            uint8_t chd[8];
            if (f.read(chd, 8) != 8) break;
            uint32_t sz = chd[4] | (uint32_t)chd[5] << 8 | (uint32_t)chd[6] << 16 | (uint32_t)chd[7] << 24;
            if (!memcmp(chd, "fmt ", 4)) {
                uint8_t fmtb[16] = {0};
                f.read(fmtb, sz > 16 ? 16 : sz);
                ch = fmtb[2] | fmtb[3] << 8;
                rate = fmtb[4] | fmtb[5] << 8 | fmtb[6] << 16 | fmtb[7] << 24;
                bits = fmtb[14] | fmtb[15] << 8;
                if (sz > 16) f.seek(f.position() + sz - 16);
            } else if (!memcmp(chd, "data", 4)) {
                dataOff = f.position();
                dataSize = sz;
                found = true;
                break;
            } else {
                f.seek(f.position() + sz + (sz & 1));
            }
        }
        if (!found || bits != 16 || ch < 1 || ch > 2 || !AudioOut::begin(rate)) { f.close(); return; }
        playChannels = ch;
        f.seek(dataOff);
        playTotal = dataSize;
        playPos = 0;
        playIsMp3 = false;
    }

    playFile = f;
    playActive = true;
    playing = true;
    String nm = path.substring(path.lastIndexOf('/') + 1);
    snprintf(playTitle, sizeof(playTitle), "%s", nm.c_str());
}

void Ui::stopPlayback() {
    if (!playing) return;
    playActive = false;
    AudioOut::stop();
    if (playFile) playFile.close();
    playing = false;
    playTotal = 0;
    playPos = 0;
}

void Ui::tickPlayback() {
    if (!playing || !playFile) { playActive = playing; return; }
    playActive = true;

    if (playIsMp3) {
        // 3-4 frames per tick (~60-90 ms of audio) — the DMA does not starve
        for (int frame = 0; frame < 4 && playing; frame++) {
            refillMp3();
            if (mp3Fill == 0 && !playFile.available()) { stopPlayback(); return; }
            mp3dec_frame_info_t info;
            static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples = mp3dec_decode_frame(&mp3dec, mp3buf, mp3Fill, pcm, &info);
            if (info.frame_bytes == 0) {
                // frame did not fit / was not recognized
                if (mp3Fill == MP3_BUF) {
                    memmove(mp3buf, mp3buf + MP3_BUF / 2, MP3_BUF / 2);
                    mp3Fill = MP3_BUF / 2;
                }
                refillMp3();
                if (mp3Fill == 0 && !playFile.available()) { stopPlayback(); return; }
                return; // top up with data on the next tick
            }
            if (samples > 0 && info.channels > 0) {
                if (info.hz > 0) AudioOut::begin(info.hz);
                int ch = info.channels;
                if (ch == 2) {
                    AudioOut::write(pcm, samples * 2);
                } else {
                    static int16_t stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
                    for (int i = 0; i < samples; i++) { stereo[i * 2] = pcm[i]; stereo[i * 2 + 1] = pcm[i]; }
                    AudioOut::write(stereo, samples * 2);
                }
            }
            memmove(mp3buf, mp3buf + info.frame_bytes, mp3Fill - info.frame_bytes);
            mp3Fill -= info.frame_bytes;
        }
        return;
    }

    // WAV: read a chunk, duplicate mono into stereo
    static int16_t wavBuf[1024];
    int n = playFile.read((uint8_t*)wavBuf, sizeof(wavBuf));
    if (n <= 0) { stopPlayback(); return; }
    int values = n / 2;
    playPos += n;
    if (playChannels == 2) {
        AudioOut::write(wavBuf, values);
    } else {
        static int16_t stereo[2048];
        for (int i = 0; i < values; i++) { stereo[i * 2] = wavBuf[i]; stereo[i * 2 + 1] = wavBuf[i]; }
        AudioOut::write(stereo, values * 2);
    }
    if (playTotal && playPos >= playTotal) { stopPlayback(); return; }
}

// =====================================================================
//  Navigation: categories / items (logic does not depend on the visuals)
// =====================================================================
int Ui::sectionTotal() const { return Features::categoryCount(); }

// The list shows only categories with items; position -> category.
void Ui::rebuildVisibleCats() {
    visibleCatCount = 0;
    for (int c = 0; c < sectionTotal(); c++)
        if (itemCount(c) > 0) visibleCat[visibleCatCount++] = c;
    // sidebar order is explicit (see Features::categoryOrder / MwOrder)
    for (int i = 1; i < visibleCatCount; i++) {
        int v = visibleCat[i];
        int j = i - 1;
        while (j >= 0 && Features::categoryOrder(visibleCat[j]) > Features::categoryOrder(v)) {
            visibleCat[j + 1] = visibleCat[j];
            j--;
        }
        visibleCat[j + 1] = v;
    }
    if (visibleCatCount == 0) visibleCat[visibleCatCount++] = 0; // guard against an empty menu
    if (sidebarIndex >= visibleCatCount) sidebarIndex = 0;
}

int Ui::catOf(int pos) const {
    if (pos < 0 || pos >= visibleCatCount) return 0;
    return visibleCat[pos];
}

int Ui::builtinTabCount(Section s) const {
    switch (s) {
        case Section::Wifi:      return 1;  // only Connect; WebUI is under Files
        case Section::Files:     return 3;  // SD / Flash / WebUI
        case Section::Settings:  return 5;
        default:                 return 0; // SubGhz — entirely registry features
    }
}

const char* Ui::builtinTabName(Section s, int i) const {
    static const char* wf[]   = {"Connect"};
    static const char* fl[]   = {"SD", "Flash", "WebUI"};
    static const char* set[]  = {"Customize", "Time", "About", "Sleep", "Reboot"};
    switch (s) {
        case Section::Wifi:      return wf[i];
        case Section::Files:     return fl[i];
        case Section::Settings:  return set[i];
        default:                 return "";
    }
}

int Ui::itemCount(int sectionIdx) const {
    int builtin = sectionIdx < (int)Section::COUNT ? builtinTabCount((Section)sectionIdx) : 0;
    return builtin + Features::featureCount(sectionIdx);
}

const char* Ui::itemName(int sectionIdx, int i) const {
    int builtin = sectionIdx < (int)Section::COUNT ? builtinTabCount((Section)sectionIdx) : 0;
    if (i < builtin) return builtinTabName((Section)sectionIdx, i);
    const Feature* f = Features::feature(sectionIdx, i - builtin);
    return f ? f->name : "";
}

bool Ui::itemIsFeature(int sectionIdx, int i) const {
    int builtin = sectionIdx < (int)Section::COUNT ? builtinTabCount((Section)sectionIdx) : 0;
    return i >= builtin;
}

bool Ui::sectionFieldEditable(Section s) const {
    return s == Section::Settings || s == Section::Wifi || s == Section::Files;
}

// =====================================================================
//  Initialization
// =====================================================================
void Ui::begin() {
    Features::init(); // lay features out by category (after all static constructors)
    rebuildVisibleCats(); // hide categories without items (Bluetooth disabled etc.)

    // Display settings survive reboot (NVS)
    displayBrightness = (uint8_t)constrain(Config::getInt("disp_bright", 180), 10, 255);
    displayAutoDimSec = Config::getInt("disp_dim", 60);
    {
        static const int presets[] = {0, 15, 30, 60, 120, 300};
        bool ok = false;
        for (int i = 0; i < 6; i++) if (presets[i] == displayAutoDimSec) ok = true;
        if (!ok) displayAutoDimSec = 60;
    }
    // Theme customization: accent + footer hints
    themeAccentIdx = (uint8_t)constrain(Config::getInt("theme_accent", 0), 0, (int)Theme::ACCENT_PRESETS - 1);
    Theme::setAccent(themeAccentIdx);
    footerHints = Config::getBool("footer_hints", true);
    // Screen orientation and time zone also survive reboot
    displayFlipped = Config::getBool("disp_flip", false);
    utcOffsetHours = constrain(Config::getInt("utc_off", 0), -12, 14);
    // LED customization (encoder): everything saveLedSettings() persists
    ledBrightness = (uint8_t)constrain(Config::getInt("led_bright", 90), 0, 255);
    ledR = (uint8_t)constrain(Config::getInt("led_r", 214), 0, 255);
    ledG = (uint8_t)constrain(Config::getInt("led_g", 24), 0, 255);
    ledB = (uint8_t)constrain(Config::getInt("led_b", 34), 0, 255);
    ledModeIdx = constrain(Config::getInt("led_mode", 2), 0, 4);
    ledSpeedPct = (uint8_t)constrain(Config::getInt("led_speed", 100), 10, 300);
    Board::setLedBrightness(ledBrightness);
    Board::setLedColor(ledR, ledG, ledB);
    Board::setLedMode((LedMode)ledModeIdx);
    Board::setLedSpeed(ledSpeedPct);
    Board::setLedClickBlip(Config::getBool("led_click", true));
    Board::setLedRotGlow(Config::getBool("led_rot", true));

    display.begin();
    display.setRotation(3);
    if (displayFlipped) applyDisplayRotation();
    display.setBrightness(displayBrightness);
    display.fillScreen(Theme::BG);

    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(Theme::SCREEN_W, Theme::SCREEN_H);

    bootAt = millis();
    lastActivityMs = millis();
    drawSplash();
    canvas.pushSprite(0, Y_OFFSET);
}

void Ui::applyDisplayRotation() {
    display.setRotation(displayFlipped ? 1 : 3);
}

// ---- entry points from WebUI (Ui fields are private — change via methods) ----
void Ui::webSetBrightness(uint8_t v) {
    displayBrightness = (uint8_t)constrain((int)v, 10, 255);
    display.setBrightness(displayBrightness);
    Config::setInt("disp_bright", displayBrightness);
}

bool Ui::webSetAccentPreset(int idx) {
    if (idx < 0 || idx >= Theme::ACCENT_PRESETS) return false;
    themeAccentIdx = (uint8_t)idx;
    Theme::setAccent(themeAccentIdx);
    Config::setInt("theme_accent", themeAccentIdx);
    return true;
}

void Ui::saveLedSettings() {
    Config::setInt("led_bright", ledBrightness);
    Config::setInt("led_r", ledR);
    Config::setInt("led_g", ledG);
    Config::setInt("led_b", ledB);
    Config::setInt("led_mode", ledModeIdx);
    Config::setInt("led_speed", ledSpeedPct);
    Config::setBool("led_click", Board::getLedClickBlip());
    Config::setBool("led_rot", Board::getLedRotGlow());
}

// ---- flash a .bin straight from storage (hold on a file in Files) ----
// Picks the next OTA partition, writes block by block, reboots on success.
// The warning screen is drawn BEFORE flashing starts: the process is blocking.
bool Ui::flashFirmwareFile(FsRoot root, const String& path) {
    fs::FS& fsRef = root == FsRoot::SD ? (fs::FS&)SD : (fs::FS&)LittleFS;
    File f = fsRef.open(path);
    if (!f || f.isDirectory()) { f.close(); return false; }
    size_t sz = f.size();
    const esp_partition_t* next = esp_ota_get_next_update_partition(esp_ota_get_running_partition());
    const char* err = nullptr;
    if (!next) err = "no OTA partition - flash via cable once";
    else if (sz == 0 || sz > next->size) err = "file too big for OTA slot";

    canvas.fillSprite(Theme::BG);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString("FLASHING - DO NOT POWER OFF", 10, 20);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString(path, 10, 40);
    if (err) {
        canvas.setTextColor(Theme::ERR, Theme::BG);
        canvas.drawString(err, 10, 70);
        canvas.pushSprite(0, Y_OFFSET);
        f.close();
        delay(4000);
        return false;
    }
    canvas.pushSprite(0, Y_OFFSET);

    if (!Update.begin(next->size)) { err = Update.errorString(); }
    else {
        static uint8_t buf[4096];
        while (f.available()) {
            int n = f.read(buf, sizeof(buf));
            if (n <= 0 || Update.write(buf, (size_t)n) != (size_t)n) { err = Update.errorString(); break; }
        }
        if (!err && !Update.end(true)) err = Update.errorString();
    }
    f.close();
    if (!err) { delay(300); ESP.restart(); }

    canvas.fillRect(0, 60, Theme::SCREEN_W, 24, Theme::BG);
    canvas.setTextColor(Theme::ERR, Theme::BG);
    canvas.drawString("FLASH FAILED:", 10, 64);
    canvas.drawString(err ? err : "?", 10, 78);
    canvas.pushSprite(0, Y_OFFSET);
    delay(4000);
    return false;
}

void Ui::noteActivity(const InputState& in) {
    if (in.rotationSteps != 0 || in.event != InputEvent::None) {
        lastActivityMs = millis();
        if (displayDimmedNow) {
            display.setBrightness(displayBrightness);
            displayDimmedNow = false;
        }
    }
    
}

// =====================================================================
//  Main tick
// =====================================================================
void Ui::tick(const InputState& in) {
    if (!apPresetLoaded) {
        int saved = Config::getInt("ap_preset", 0);
        apPresetIdx = (saved >= 0 && saved < 3) ? saved : 0;
        apPresetLoaded = true;
    }
    if (!splashDone) {
        if (!splashStarted) { bootAt = millis(); splashStarted = true; }
        if (millis() - bootAt > 1100) splashDone = true;
        drawSplash();
        canvas.pushSprite(0, Y_OFFSET);
        return;
    }

    noteActivity(in);

    // auto-dim: after displayAutoDimSec seconds of idle — then a staircase
    // of brightness down to zero (configured in Customize > Display > Auto-dim;
    // any input restores the user brightness)
    {
        static int dimStage = -1; // -1 = not dimmed
        int newStage = -1;
        if (displayAutoDimSec > 0) {
            uint32_t idleMs = millis() - lastActivityMs;
            uint32_t base = (uint32_t)displayAutoDimSec * 1000UL;
            if      (idleMs > base + 21000) newStage = 4; // off
            else if (idleMs > base + 15000) newStage = 3;
            else if (idleMs > base + 9000)  newStage = 2;
            else if (idleMs > base + 3000)  newStage = 1;
            else if (idleMs > base)         newStage = 0;
        }
        if (newStage != dimStage) {
            dimStage = newStage;
            displayDimmedNow = (dimStage >= 0);
            static const uint8_t PCT[] = {55, 35, 20, 10, 0}; // % of the user brightness
            display.setBrightness(dimStage < 0 ? displayBrightness
                                               : (uint8_t)((uint16_t)displayBrightness * PCT[dimStage] / 100));
        }
    }

    // Encoder LED feedback (on/off in Customize > LED)
    if (in.event == InputEvent::Click) Board::ledBlip();
    if (in.rotationSteps != 0) Board::ledRot();

    // web-sweep toggled from the browser — redraw the header so the
    // RF SWEEP indicator appears/disappears on the device
    static bool lastWebSweep = false;
    if (SubGhz::webSpectrumActive() != lastWebSweep) {
        lastWebSweep = SubGhz::webSpectrumActive();
        frameMoving = true;
        lastFrameAnimated = true;
    }

    // player: feed I2S on every tick BEFORE any early exits
    if (playing) tickPlayback();

    // Wi-Fi RAM is released automatically once a minute (the function
    // itself does not touch the connected network/AP/scan)
    static uint32_t lastRamSweep = 0;
    if (millis() - lastRamSweep > 60000) {
        lastRamSweep = millis();
        if (!playing && !WifiMgr::isScanning() &&
            WifiMgr::connState() != WifiConnState::Connecting) {
            WifiMgr::releaseIdleResources();
        }
    }
    // internet check requested by About (runs here, not in the drawing code)
    if (netPending) netCheckTick();

    // captive portal: serves clients while enabled (works in the background)
    Portal::loop();

    // web spectrum analyzer: sweep on request from WebUI
    if (SubGhz::webSpectrumActive()) SubGhz::sweepOnce();

    if (in.powerHeldMs > 250) {
        drawScreen();
        drawPowerCountdown(in.powerHeldMs);
        canvas.pushSprite(0, Y_OFFSET);
        // After release, the frame with the "DEEP SLEEP IN n/3" overlay must
        // be redrawn — holding longer than CLICK_MAX_MS creates no event.
        lastFrameAnimated = true;
        return;
    }

    // --- background tasks: every tick, regardless of redrawing ---
    if (WifiMgr::isConnected()) {
        if (!timeConfigured) {
            configTime(utcOffsetHours * 3600, 0, "pool.ntp.org");
            timeConfigured = true;
        }
    } else {
        timeConfigured = false;
    }
    // Previously the web was served only with an STA connection — in own
    // access-point (AP) mode the WebUI froze.
    // Heap fuse: internal RAM is shared by the Wi-Fi stack, WebUI and
    // scripts. On exhaustion WebUI goes first — otherwise everything
    // dies at once (the UI stops responding, the radio falls apart).
    if (WebUI::isRunning() && ESP.getFreeHeap() < 24576) {
        Serial.println("[MEGAWARE] low heap: stopping WebUI (feature/scripts need RAM)");
        WebUI::stop();
        webUiEnabled = false;
    }
    if (WebUI::isRunning()) WebUI::loop();

    // --- decide whether a frame is needed ---
    const bool inputActive = (in.rotationSteps != 0 || in.event != InputEvent::None);
    uint32_t now = millis();
    // Periodic refresh: "live" screens (SubGhz/features: RSSI/animations;
    // an active feature) — once a second. Static ones (menu/settings/files)
    // change only on input or a Wi-Fi state change — clock/battery in the
    // header is fine once a minute: a full frame is 106 KB over the shared SPI bus.
    int periodicCat = catOf(sidebarIndex);
    const bool screenStatic = focusLevel != FocusLevel::Feature &&
        (periodicCat == (int)Section::Wifi ||
         periodicCat == (int)Section::Files || periodicCat == (int)Section::Settings);
    bool periodic = (now - lastPeriodicDrawMs) >= (screenStatic ? 60000U : 1000U);
    if (periodic) lastPeriodicDrawMs = now;
    const bool featureRedraw = Features::takeRedrawRequest(); // features request a frame

    WifiConnState cs = WifiMgr::connState();
    bool scanning = WifiMgr::isScanning();
    const bool stateChanged = (cs != lastConnState) || (scanning != lastScanning);
    lastConnState = cs;
    lastScanning = scanning;

    if (focusLevel == FocusLevel::Modal) {
        handleModalInput(in);
        if (inputActive || periodic || lastFrameAnimated || stateChanged) {
            frameMoving = false;
            if (modalKind == ModalKind::Reader) drawModalReader();
            else drawModalWifiKeyboard();
            lastFrameAnimated = frameMoving;
            canvas.pushSprite(0, Y_OFFSET);
        }
        return;
    }

    if (focusLevel == FocusLevel::Feature && activeFeature) {
        // input -> feature tick -> frame on demand
        handleFeatureInput(in);
        if (focusLevel != FocusLevel::Feature || !activeFeature) {
            // the feature exited via Quit — draw the screen we returned to,
            // otherwise activeFeature, already nulled, is dereferenced below (crash!)
            frameMoving = false;
            drawScreen();
            lastFrameAnimated = frameMoving;
            canvas.pushSprite(0, Y_OFFSET);
            return;
        }
        if (inputActive || periodic || lastFrameAnimated || stateChanged || featureRedraw) {
            frameMoving = false;
            drawHeader();
            int x0, y0, w, h;
            contentRect(x0, y0, w, h);
            canvas.setTextSize(Theme::SIZE_TEXT);
            if (activeFeature->onDraw) activeFeature->onDraw(canvas, x0, y0, w, h);
            if (footerHints) drawFooter("select", "ok", "back");
            lastFrameAnimated = false;
            canvas.pushSprite(0, Y_OFFSET);
        }
        return;
    }

    handleInput(in);

    if (!inputActive && !periodic && !lastFrameAnimated && !stateChanged && !featureRedraw) return;

    frameMoving = false;
    drawScreen();
    lastFrameAnimated = frameMoving;
    canvas.pushSprite(0, Y_OFFSET);
}

// =====================================================================
//  Frame skeleton: header + content per focus level + footer
// =====================================================================
void Ui::drawScreen() {
    drawHeader();

    int x0, y0, w, h;
    contentRect(x0, y0, w, h);

    if (focusLevel == FocusLevel::Feature && activeFeature && activeFeature->onDraw) {
        canvas.setTextSize(Theme::SIZE_TEXT); // features start at the base text size
        activeFeature->onDraw(canvas, x0, y0, w, h);
    } else if (focusLevel == FocusLevel::Sidebar) {
        drawMainMenu();
    } else if (focusLevel == FocusLevel::SubTab) {
        drawSectionList();
    } else {
        drawFieldScreen();
    }

    // Footer hints — context-dependent
    const char* tv = "select", *pv = "ok", *hv = "back";
    if (footerHints) {
        if (focusLevel == FocusLevel::Field) {
            int cat = catOf(sidebarIndex);
            int tab = subTabIndex[cat];
            if (cat == (int)Section::Settings && tab <= 1 && (ledEditingRow || displayEditingRow)) { tv = "value"; pv = "done"; }
            else if (cat == (int)Section::Settings && tab == 1) { tv = "offset"; pv = "done"; }
            else if (cat == (int)Section::Settings && tab == 2) { tv = "scroll"; pv = "-"; }
            else if (cat == (int)Section::Settings && tab == 0 && customizeOpen == 2) { pv = "change"; }
            else if (cat == (int)Section::Settings && tab == 0 && customizeOpen >= 0) { tv = "value"; pv = "done"; }
        }
        drawFooter(tv, pv, hv);
    }
}

void Ui::drawPowerCountdown(uint32_t heldMs) {
    int sec = (int)(heldMs / 1000) + 1;
    if (sec > 3) sec = 3;
    char buf[24];
    snprintf(buf, sizeof(buf), "DEEP SLEEP IN %d/3", sec);

    canvas.fillRect(0, 0, Theme::SCREEN_W, Theme::HEADER_H, Theme::BG);
    canvas.drawFastHLine(0, Theme::HEADER_H, Theme::SCREEN_W, Theme::ACCENT);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString(buf, Theme::SCREEN_W / 2, Theme::HEADER_H / 2);
    canvas.setTextSize(Theme::SIZE_TEXT);
}

void Ui::showShutdown() {
    canvas.fillSprite(Theme::BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString("MEGAWARE", Theme::SCREEN_W / 2, 70);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString("Powering off...", Theme::SCREEN_W / 2, 100);
    canvas.pushSprite(0, Y_OFFSET);
}

void Ui::performDashboardAction(int index) {
    if (index == 0) {
        showShutdown();
        delay(600);
        Board::enterDeepSleep();
    } else {
        canvas.fillSprite(Theme::BG);
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.setTextSize(Theme::SIZE_MENU);
        canvas.setTextColor(Theme::ACCENT, Theme::BG);
        canvas.drawString("REBOOTING...", Theme::SCREEN_W / 2, 85);
        canvas.setTextSize(Theme::SIZE_TEXT);
        canvas.pushSprite(0, Y_OFFSET);
        delay(400);
        ESP.restart();
    }
}

// =====================================================================
//  Input — menu / items / rows (Modal and Feature are handled separately)
// =====================================================================
void Ui::handleInput(const InputState& in) {
    if (in.event == InputEvent::None && in.rotationSteps == 0) return;

    int cat = catOf(sidebarIndex); // list position -> registry category
    Section cur = (Section)cat;
    int count = itemCount(cat);

    if (focusLevel == FocusLevel::Field) {
        // Field is reachable only in built-in sections (features receive
        // input themselves, see FocusLevel::Feature) — a guard for a new category.
        if (cat >= (int)Section::COUNT) { focusLevel = FocusLevel::SubTab; return; }
        if (cur == Section::Settings) {
            int tab = subTabIndex[cat];

            if (tab == 0) { // Customize
                if (customizeOpen < 0) { // list of LED/Display/Theme
                    constexpr int ROWS = 3;
                    if (in.rotationSteps != 0) {
                        int idx = fieldIndex[cat];
                        idx = ((idx + in.rotationSteps) % ROWS + ROWS) % ROWS;
                        fieldIndex[cat] = idx;
                    }
                    if (in.event == InputEvent::Click) {
                        ledEditingRow = false;   // enter the sublist clean:
                        displayEditingRow = false; // no stuck edit mode
                        customizeOpen = fieldIndex[cat];
                    }
                    if (in.event == InputEvent::UserKey) focusLevel = FocusLevel::SubTab;
                    return;
                }
                if (customizeOpen == 0) { // LED — Brightness / R / G / B / Mode / Speed / Click blip / Rotate glow
                    constexpr int ROWS = 8;
                    if (!ledEditingRow) {
                        if (in.rotationSteps != 0) {
                            int idx = fieldIndex[cat];
                            idx = ((idx + in.rotationSteps) % ROWS + ROWS) % ROWS;
                            fieldIndex[cat] = idx;
                        }
                        if (in.event == InputEvent::Click) ledEditingRow = true;
                        if (in.event == InputEvent::UserKey) customizeOpen = -1;
                    } else {
                        if (in.rotationSteps != 0) {
                            int row = fieldIndex[cat];
                            int step = in.rotationSteps;
                            switch (row) {
                                case 0: ledBrightness = (uint8_t)constrain((int)ledBrightness + step * 15, 0, 255);
                                        Board::setLedBrightness(ledBrightness); break;
                                case 1: ledR = (uint8_t)constrain((int)ledR + step * 8, 0, 255);
                                        Board::setLedColor(ledR, ledG, ledB); break;
                                case 2: ledG = (uint8_t)constrain((int)ledG + step * 8, 0, 255);
                                        Board::setLedColor(ledR, ledG, ledB); break;
                                case 3: ledB = (uint8_t)constrain((int)ledB + step * 8, 0, 255);
                                        Board::setLedColor(ledR, ledG, ledB); break;
                                case 4: { int m = ((ledModeIdx + step) % 5 + 5) % 5; ledModeIdx = m;
                                          Board::setLedMode((LedMode)m); break; }
                                case 5: ledSpeedPct = (uint8_t)constrain((int)ledSpeedPct + step * 10, 10, 300);
                                        Board::setLedSpeed(ledSpeedPct); break;
                                case 6: Board::setLedClickBlip(!Board::getLedClickBlip()); break;
                                case 7: Board::setLedRotGlow(!Board::getLedRotGlow()); break;
                            }
                        }
                        if (in.event == InputEvent::Click) ledEditingRow = false;
                        if (in.event == InputEvent::UserKey) {
                            saveLedSettings(); // all LED settings survive reboot
                            ledEditingRow = false;
                            customizeOpen = -1;
                        }
                    }
                    return;
                }
                if (customizeOpen == 1) { // Display — Brightness / Auto-dim / Rotation
                    constexpr int ROWS = 3;
                    if (!displayEditingRow) {
                        if (in.rotationSteps != 0) {
                            int idx = fieldIndex[cat];
                            idx = ((idx + in.rotationSteps) % ROWS + ROWS) % ROWS;
                            fieldIndex[cat] = idx;
                        }
                        if (in.event == InputEvent::Click) displayEditingRow = true;
                        if (in.event == InputEvent::UserKey) customizeOpen = -1;
                    } else {
                        if (in.rotationSteps != 0) {
                            int row = fieldIndex[cat];
                            int step = in.rotationSteps;
                            if (row == 0) {
                                displayBrightness = (uint8_t)constrain((int)displayBrightness + step * 15, 10, 255);
                                display.setBrightness(displayBrightness);
                            } else if (row == 1) {
                                // auto-dim presets: off / 15 / 30 / 60 / 120 / 300 s
                                static const int presets[] = {0, 15, 30, 60, 120, 300};
                                int cur = 3;
                                for (int i = 0; i < 6; i++) if (presets[i] == displayAutoDimSec) cur = i;
                                displayAutoDimSec = presets[((cur + step) % 6 + 6) % 6];
                            } else {
                                if (step != 0) { displayFlipped = !displayFlipped; applyDisplayRotation(); }
                            }
                        }
                        if (in.event == InputEvent::Click || in.event == InputEvent::UserKey) {
                            displayEditingRow = false;
                            Config::setInt("disp_bright", displayBrightness);
                            Config::setInt("disp_dim", displayAutoDimSec);
                            Config::setBool("disp_flip", displayFlipped);
                        }
                    }
                    return;
                }
                { // customizeOpen == 2: Theme — a click changes the value in place
                    constexpr int ROWS = 2;
                    if (fieldIndex[cat] >= ROWS) fieldIndex[cat] = 0;
                    if (in.rotationSteps != 0) {
                        int idx = fieldIndex[cat];
                        idx = ((idx + in.rotationSteps) % ROWS + ROWS) % ROWS;
                        fieldIndex[cat] = idx;
                    }
                    if (in.event == InputEvent::Click) {
                        if (fieldIndex[cat] == 0) {
                            themeAccentIdx = (themeAccentIdx + 1) % Theme::ACCENT_PRESETS;
                            Theme::setAccent(themeAccentIdx);
                            Config::setInt("theme_accent", themeAccentIdx);
                        } else {
                            footerHints = !footerHints;
                            Config::setBool("footer_hints", footerHints);
                        }
                    }
                    if (in.event == InputEvent::UserKey) customizeOpen = -1;
                    return;
                }
                return;
            }

            if (tab == 1) { // Time
                if (in.rotationSteps != 0) {
                    utcOffsetHours = constrain(utcOffsetHours + in.rotationSteps, -12, 14);
                    timeConfigured = false;
                }
                if (in.event == InputEvent::Click || in.event == InputEvent::UserKey) {
                    Config::setInt("utc_off", utcOffsetHours); // time zone survives reboot
                    focusLevel = FocusLevel::SubTab;
                }
                return;
            }

            if (tab == 2) { // About — scrolling the long list
                int x0, y0, w, h;
                contentRect(x0, y0, w, h);
                int visible = (h - 20) / Theme::ROW_LIST; // in sync with drawAboutScreen()
                if (visible < 1) visible = 1;
                int top = fieldIndex[cat] + in.rotationSteps;
                int maxTop = aboutRowCount() - visible;
                if (top > maxTop) top = maxTop;
                if (top < 0) top = 0;
                fieldIndex[cat] = top;
                if (in.event == InputEvent::Click || in.event == InputEvent::UserKey) focusLevel = FocusLevel::SubTab;
                return;
            }
            return;
        }

        if (cur == Section::Wifi) {
            int tab = subTabIndex[cat];
            if (tab == 0) { // Connect — network list; hold OK — rescan
                int n = WifiMgr::resultCount();
                if (n > 0 && in.rotationSteps != 0) {
                    int idx = fieldIndex[cat];
                    idx = ((idx + in.rotationSteps) % n + n) % n;
                    fieldIndex[cat] = idx;
                }
                if (in.event == InputEvent::Click && n > 0) {
                    WifiNetwork net = WifiMgr::result(fieldIndex[cat]);
                    if (net.secure) {
                        modalKind = ModalKind::WifiPassword;
                        modalTargetSSID = net.ssid;
                        pwBuffer = "";
                        kbIndex = 0;
                        kbMode = 0;
                        focusLevel = FocusLevel::Modal;
                    } else {
                        WifiMgr::connect(net.ssid, "");
                    }
                }
                if (in.event == InputEvent::LongPress) {
                    WifiMgr::startScan();
                }
            }
            if (in.event == InputEvent::UserKey) focusLevel = FocusLevel::SubTab;
            return;
        }

        if (cur == Section::Files) {
            if (subTabIndex[cat] == 2) { // WebUI — status + toggle
                if (in.event == InputEvent::Click) {
                    webUiEnabled = !webUiEnabled;
                    if (webUiEnabled) {
                        if (Portal::isRunning()) Portal::stop();
                        if (!WifiMgr::isAPRunning() && !WifiMgr::isConnected()) WifiMgr::startAP(0);
                        WebUI::start();
                    } else {
                        WebUI::stop();
                        if (WifiMgr::isAPRunning()) WifiMgr::stopAP();
                        WifiMgr::releaseIdleResources();
                    }
                    lastFrameAnimated = true;
                }
                if (in.event == InputEvent::UserKey) focusLevel = FocusLevel::SubTab;
                return;
            }
            FsRoot root = subTabIndex[cat] == 0 ? FsRoot::SD : FsRoot::Flash;
            String& path = filesPath[subTabIndex[cat]];
            int n = Files::listCount(root, path);

            // Quit while a track is playing — stop first, navigation on the second Quit
            if (playing && in.event == InputEvent::UserKey) {
                stopPlayback();
                lastFrameAnimated = true;
                return;
            }
            // Quit inside a directory — one level UP; from the root — to Files
            if (in.event == InputEvent::UserKey && path != "/") {
                int slash = path.substring(0, path.length() - 1).lastIndexOf('/');
                path = slash > 0 ? path.substring(0, slash) : "/";
                fieldIndex[cat] = 0;
                return;
            }

            if (n > 0 && in.rotationSteps != 0) {
                int idx = fieldIndex[cat];
                idx = ((idx + in.rotationSteps) % n + n) % n;
                fieldIndex[cat] = idx;
            }
            if (in.event == InputEvent::Click && n > 0) {
                FsEntry e = Files::listEntry(root, path, fieldIndex[cat]);
                String ename(e.name);
                if (e.isDir) {
                    if (!path.endsWith("/")) path += "/";
                    path += ename;
                    fieldIndex[cat] = 0;
                } else if (ename.endsWith(".lua") || ename.endsWith(".js")) {
                    String full = path;
                    if (!full.endsWith("/")) full += "/";
                    full += ename;
                    bool ok = ename.endsWith(".lua") ? LuaEngine::runFile(full) : JsEngine::runFile(full);
                    lastScriptResult = ok ? "OK: " + ename
                                           : "ERR: " + (ename.endsWith(".lua") ? LuaEngine::lastError() : JsEngine::lastError());
                    // show the result right away and fade it out after 4 s
                    lastScriptResultAt = millis();
                    lastFrameAnimated = true;
                }
            }
            if (in.event == InputEvent::LongPress) {
                // hold on an audio file — play, hold on .bin — flash,
                // otherwise — up the folder tree
                FsEntry e = Files::listEntry(root, path, fieldIndex[cat]);
                String ename(e.name);
                String lower = ename; lower.toLowerCase();
                if (n > 0 && lower.endsWith(".bin")) {
                    String full = path;
                    if (!full.endsWith("/")) full += "/";
                    full += e.name;
                    flashFirmwareFile(root, full);
                    lastFrameAnimated = true;
                    return;
                }
                if (n > 0 && (lower.endsWith(".mp3") || lower.endsWith(".wav"))) {
                    String full = path;
                    if (!full.endsWith("/")) full += "/";
                    full += e.name;
                    startPlayback(full);
                    lastFrameAnimated = true;
                    return;
                }
                if (n > 0 && (lower.endsWith(".txt") || lower.endsWith(".md") ||
                              lower.endsWith(".log") || lower.endsWith(".pdf"))) {
                    String full = path;
                    if (!full.endsWith("/")) full += "/";
                    full += e.name;
                    openReader(root, full);
                    lastFrameAnimated = true;
                    return;
                }
                if (path != "/") {
                    int slash = path.substring(0, path.length() - 1).lastIndexOf('/');
                    path = slash > 0 ? path.substring(0, slash) : "/";
                    fieldIndex[cat] = 0;
                } else {
                    focusLevel = FocusLevel::SubTab;
                }
            }
            if (in.event == InputEvent::UserKey) focusLevel = FocusLevel::SubTab;
            return;
        }
        return;
    }

    if (focusLevel == FocusLevel::SubTab) {
        if (count == 0) { focusLevel = FocusLevel::Sidebar; return; }
        if (in.rotationSteps != 0) {
            int idx = subTabIndex[cat];
            idx = ((idx + in.rotationSteps) % count + count) % count;
            subTabIndex[cat] = idx;
        }
        if (in.event == InputEvent::Click) {
            int idx = subTabIndex[cat];
            if (itemIsFeature(cat, idx)) {
                // feature item: 120 ms guard — do not enter it by accident
                if (millis() - subTabEnteredAt < 120) return;
                int builtin = cat < (int)Section::COUNT ? builtinTabCount(cur) : 0;
                enterFeature(Features::feature(cat, idx - builtin));
            } else if (cur == Section::Files && idx == 2) {
                // WebUI: a click opens the status screen (Field)
                focusLevel = FocusLevel::Field;
            } else if (cur == Section::Settings && idx >= 3) {
                // Sleep / Reboot — actions, not entering Field
                performDashboardAction(idx - 3);
                return;
            } else if (sectionFieldEditable(cur)) {
                // 120 ms guard: do not accidentally enter Field with a second click
                if (millis() - subTabEnteredAt < 120) return;
                focusLevel = FocusLevel::Field;
                if (cur == Section::Settings) customizeOpen = -1;
                if (cur == Section::Wifi && subTabIndex[cat] == 0) {
                    if (!WifiMgr::isScanning()) {
                        WifiMgr::tryAutoConnect();
                        if (WifiMgr::resultCount() == 0 && !WifiMgr::isScanning()) WifiMgr::startScan();
                    }
                    wifiScanTriggered = true;
                }
            }
        }
        if (in.event == InputEvent::UserKey) {
            if (cur == Section::Wifi && !webUiEnabled) WifiMgr::releaseIdleResources();
            focusLevel = FocusLevel::Sidebar;
        }
        return;
    }

    int total = visibleCatCount;
    if (in.rotationSteps != 0) {
        sidebarIndex = ((sidebarIndex + in.rotationSteps) % total + total) % total;
    }
    if (in.event == InputEvent::Click && itemCount(catOf(sidebarIndex)) > 0) {
        focusLevel = FocusLevel::SubTab;
        subTabEnteredAt = millis();
    }
}

// =====================================================================
//  Registry features: enter / input / exit
// =====================================================================
void Ui::enterFeature(const Feature* f) {
    if (!f) return;
    activeFeature = f;
    focusLevel = FocusLevel::Feature;
    if (f->onEnter) f->onEnter();
}

void Ui::handleFeatureInput(const InputState& in) {
    if (!activeFeature) { focusLevel = FocusLevel::SubTab; return; }

    bool consumed = false;
    if (activeFeature->onInput && (in.event != InputEvent::None || in.rotationSteps != 0))
        consumed = activeFeature->onInput(in);

    // An unconsumed Quit — exit the feature to the item-list level (GUI
    // convention: Quit = one level up). LongPress and the rest are the feature's job.
    if (!consumed && in.event == InputEvent::UserKey) {
        if (activeFeature->onExit) activeFeature->onExit();
        activeFeature = nullptr;
        focusLevel = FocusLevel::SubTab;
        return;
    }

    if (activeFeature->onTick) activeFeature->onTick();
}

// =====================================================================
//  Modal: input — on-screen QWERTY / WebUI settings (logic unchanged)
// =====================================================================
void Ui::handleModalInput(const InputState& in) {
    if (modalKind == ModalKind::Reader) {
        // book navigation: rotation = smooth line scroll, click = page
        // forward, hold = page back, Quit = exit (bookmark kept in NVS)
        if (in.rotationSteps != 0)
            readerCur = constrain((int)readerCur + in.rotationSteps, 0, (int)(readerLineCount - 1));
        if (in.event == InputEvent::Click)
            readerCur = constrain((int)readerCur + 12, 0, (int)(readerLineCount - 1));
        if (in.event == InputEvent::LongPress)
            readerCur = constrain((int)readerCur - 12, 0, (int)(readerLineCount - 1));
        if (in.event == InputEvent::UserKey) closeReader();
        lastFrameAnimated = true;
        return;
    }
    if (modalKind != ModalKind::WifiPassword) { focusLevel = FocusLevel::Field; modalKind = ModalKind::None; return; }
    // guard: without input there is nothing to do — saves a String per tick
    if (in.event == InputEvent::None && in.rotationSteps == 0) return;

    const int* rowLen = kbMode == 0 ? kb::lettersRowLen : kb::symsRowLen;
    int totalGridKeys = rowLen[0] + rowLen[1] + rowLen[2];
    int total = totalGridKeys + 6;

    if (in.rotationSteps != 0) kbIndex = ((kbIndex + in.rotationSteps) % total + total) % total;

    String label;
    {
        int idx = kbIndex;
        if (idx < totalGridKeys) {
            int r = 0;
            while (idx >= rowLen[r]) { idx -= rowLen[r]; r++; }
            label = kbMode == 0 ? kb::lettersRows[r][idx] : kb::symsRows[r][idx];
        } else {
            int fi = idx - totalGridKeys;
            label = kbMode == 0 ? kb::funcRowLetters[fi] : kb::funcRowSyms[fi];
        }
    }

    if (in.event == InputEvent::Click) {
        if (label == "DEL") { if (pwBuffer.length()) pwBuffer.remove(pwBuffer.length() - 1); }
        else if (label == "TXT") { kbShowPlain = !kbShowPlain; }
        else if (label == "123") { kbMode = 1; kbIndex = 0; }
        else if (label == "ABC") { kbMode = 0; kbIndex = 0; }
        else if (label == "SPACE") { pwBuffer += ' '; }
        else if (label == "OK") {
            WifiMgr::connect(modalTargetSSID, pwBuffer);
            modalKind = ModalKind::None;
            focusLevel = FocusLevel::Field;
        } else if (label == "ESC") {
            modalKind = ModalKind::None;
            focusLevel = FocusLevel::Field;
        } else if (label == "^") {
            // clicking Shift intentionally does nothing — only holding does (below)
        } else if (label.length() == 1) {
            char ch = label[0];
            if (kbMode == 0 && kbCaps) ch = (char)toupper(ch);
            pwBuffer += ch;
        }
    }
    if (in.event == InputEvent::LongPress && label == "^") {
        kbCaps = !kbCaps;
    }
    if (in.event == InputEvent::UserKey) {
        modalKind = ModalKind::None;
        focusLevel = FocusLevel::Field;
    }
}

// =====================================================================
//  Terminal-Lite: shared elements
// =====================================================================
void Ui::drawSplash() {
    canvas.fillSprite(Theme::BG);
    uint32_t t = millis() - bootAt;

    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString("MEGAWARE", Theme::SCREEN_W / 2, 70);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString("terminal-lite  v" MW_FW_VERSION, Theme::SCREEN_W / 2, 96);

    int barW = 200, barX = (Theme::SCREEN_W - barW) / 2, barY = 124;
    canvas.fillRect(barX, barY, barW, 2, Theme::TEXT_DIM);
    canvas.fillRect(barX, barY, (int)(barW * constrain((float)t / 1000.0f, 0.0f, 1.0f)), 2, Theme::ACCENT);
}

void Ui::drawHeader() {
    canvas.fillSprite(Theme::BG);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString("MEGAWARE", Theme::PAD, 2);
    canvas.setTextSize(Theme::SIZE_TEXT);

    // web-sweep indicator: shows on the device that the CC1101 is busy with the browser
    if (SubGhz::webSpectrumActive()) {
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.setTextColor(Theme::ACCENT, Theme::BG);
        canvas.drawString("RF SWEEP", Theme::PAD + 100, Theme::HEADER_H / 2);
    }

    // right side: wifi / battery+percent / clock
    char timeBuf[8] = "--:--";
    struct tm t;
    if (timeConfigured && getLocalTime(&t, 0)) {
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
    }
    int rightX = Theme::SCREEN_W - Theme::PAD;
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.setTextColor(Theme::TEXT, Theme::BG);
    canvas.drawString(timeBuf, rightX, Theme::HEADER_H / 2);
    // the battery does not overlap the clock: time 282..312, battery+percent up to 274
    drawBatteryIcon(rightX - 84, 5);
    drawWifiIcon(rightX - 102, 6, WifiMgr::isConnected());

    canvas.drawFastHLine(0, Theme::HEADER_H - 1, Theme::SCREEN_W, Theme::ACCENT);
}

void Ui::drawBatteryIcon(int x, int y) {
    int pct = Battery::percent();
    bool charging = Battery::isCharging();
    canvas.setTextDatum(textdatum_t::middle_left);

    // body + contact
    canvas.drawRect(x, y, 16, 8, Theme::TEXT_DIM);
    canvas.fillRect(x + 16, y + 2, 2, 4, Theme::TEXT_DIM);
    if (pct >= 0) {
        uint16_t col = charging ? Theme::ACCENT : (pct > 20 ? Theme::ACCENT : Theme::WARN);
        canvas.fillRect(x + 1, y + 1, (int)(14 * (pct / 100.0f)), 6, col);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(Theme::SIZE_TEXT);
        canvas.setTextColor(charging ? Theme::ACCENT : Theme::TEXT, Theme::BG);
        canvas.drawString(buf, x + 22, y + 4);
    }
}

void Ui::drawWifiIcon(int x, int y, bool connected) {
    uint16_t col = connected ? Theme::ACCENT : Theme::TEXT_DIM;
    canvas.fillCircle(x, y + 6, 1, col);
    canvas.drawArc(x, y + 6, 4, 4, 210, 330, col);
    canvas.drawArc(x, y + 6, 7, 7, 210, 330, col);
}

void Ui::drawFooter(const char* turn, const char* press, const char* hold) {
    canvas.drawFastHLine(0, Theme::SCREEN_H - Theme::FOOTER_H, Theme::SCREEN_W, Theme::ACCENT);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    int ty = Theme::SCREEN_H - Theme::FOOTER_H + (Theme::FOOTER_H - 1) / 2 + 1;

    char buf[24];
    canvas.setTextDatum(textdatum_t::middle_left);
    snprintf(buf, sizeof(buf), "TURN: %s", turn);
    canvas.drawString(buf, Theme::PAD, ty);
    canvas.setTextDatum(textdatum_t::middle_center);
    snprintf(buf, sizeof(buf), "PRESS: %s", press);
    canvas.drawString(buf, Theme::SCREEN_W / 2, ty);
    canvas.setTextDatum(textdatum_t::middle_right);
    snprintf(buf, sizeof(buf), "HOLD: %s", hold);
    canvas.drawString(buf, Theme::SCREEN_W - Theme::PAD, ty);
    canvas.setTextDatum(textdatum_t::middle_left);
}

void Ui::contentRect(int& x, int& y, int& w, int& h) {
    x = Theme::PAD;
    y = Theme::HEADER_H + 2;
    w = Theme::SCREEN_W - Theme::PAD * 2 - Theme::SCROLLBAR_W - 2;
    h = Theme::SCREEN_H - Theme::HEADER_H - Theme::FOOTER_H - 4;
}

void Ui::drawCursor(int x, int y, bool selected) {
    if (!selected) return;
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString(">", x, y);
    canvas.setTextSize(Theme::SIZE_TEXT);
}

void Ui::drawScrollbar(int x, int y, int h, int top, int visible, int total) {
    if (total <= visible || visible <= 0 || h <= 0) return;
    canvas.fillRect(x, y, Theme::SCROLLBAR_W, h, Theme::TEXT_DIM);
    int thumbH = h * visible / total;
    if (thumbH < 6) thumbH = 6;
    if (thumbH > h) thumbH = h;
    int maxTop = total - visible;
    int ty = y + (maxTop ? (h - thumbH) * top / maxTop : 0);
    canvas.fillRect(x, ty, Theme::SCROLLBAR_W, thumbH, Theme::ACCENT);
}

void Ui::drawKV(int x, int y, int w, const char* key, const char* value, uint16_t valueCol) {
    (void)w;
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString(key, x, y);
    canvas.setTextColor(valueCol, Theme::BG);
    canvas.drawString(value, x + Theme::KEY_COL_W, y);
}

uint8_t Ui::iconForSection(int cat) const {
    switch ((Section)cat) {
        case Section::SubGhz:    return MwIcon::Antenna;
        case Section::Wifi:      return MwIcon::Wifi;
        case Section::Files:     return MwIcon::Folder;
        case Section::Bluetooth: return MwIcon::Bluetooth;
        case Section::Settings:  return MwIcon::Gear;
        default:                 return Features::categoryIcon(cat);
    }
}

const char* Ui::sectionDisplayName(int cat) const {
    switch ((Section)cat) {
        case Section::SubGhz:    return "Sub-GHz";
        case Section::Wifi:      return "Wi-Fi";
        case Section::Files:     return "Files";
        case Section::Bluetooth: return "Bluetooth";
        case Section::Settings:  return "Settings";
        default:                 return Features::categoryName(cat);
    }
}

// =====================================================================
//  Screens: main menu / section item list
// =====================================================================
void Ui::drawMainMenu() {
    int x0, y0, w, h;
    contentRect(x0, y0, w, h);

    int visible = h / Theme::ROW_MENU;
    if (visible < 1) visible = 1;
    int top = scrollWindow(0x1000, sidebarIndex, visible, visibleCatCount);

    for (int i = 0; i < visible && top + i < visibleCatCount; i++) {
        int cat = visibleCat[top + i];
        int y = y0 + i * Theme::ROW_MENU;
        bool sel = (top + i == sidebarIndex);
        uint16_t col = sel ? Theme::ACCENT : Theme::TEXT_DIM;
        drawCursor(x0, y + Theme::ROW_MENU / 2, sel);
        MwDraw::drawIcon(canvas, iconForSection(cat), x0 + 18, y + Theme::ROW_MENU / 2, col);
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(Theme::SIZE_MENU);
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.setTextColor(col, Theme::BG);
        canvas.drawString(sectionDisplayName(cat), x0 + 34, y + Theme::ROW_MENU / 2);
        canvas.setTextSize(Theme::SIZE_TEXT);
    }
    drawScrollbar(Theme::SCREEN_W - Theme::SCROLLBAR_W - 1, y0, h, top, visible, visibleCatCount);
}

void Ui::drawSectionList() {
    int cat = catOf(sidebarIndex);
    int x0, y0, w, h;
    contentRect(x0, y0, w, h);

    int count = itemCount(cat);
    int visible = h / Theme::ROW_MENU;
    if (visible < 1) visible = 1;
    int sel = subTabIndex[cat];
    int top = scrollWindow(0x2000 + cat, sel, visible, count);

    canvas.setFont(&fonts::Font0);
    for (int i = 0; i < visible && top + i < count; i++) {
        int idx = top + i;
        int y = y0 + i * Theme::ROW_MENU;
        bool s = (idx == sel);
        drawCursor(x0, y + Theme::ROW_MENU / 2, s);
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.setTextSize(Theme::SIZE_MENU);
        canvas.setTextColor(s ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
        canvas.drawString(itemName(cat, idx), x0 + 16, y + Theme::ROW_MENU / 2);
        canvas.setTextSize(Theme::SIZE_TEXT);
    }
    drawScrollbar(Theme::SCREEN_W - Theme::SCROLLBAR_W - 1, y0, h, top, visible, count);
}

// =====================================================================
//  Field-level screens (rows): LED / Display / Time / About, Wi-Fi, Files
// =====================================================================
void Ui::drawFieldScreen() {
    int cat = catOf(sidebarIndex);
    int tab = subTabIndex[cat];
    int x0, y0, w, h;
    contentRect(x0, y0, w, h);

    // item title in the accent color, content below
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_MENU);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString(itemName(cat, tab), x0, y0);
    canvas.setTextSize(Theme::SIZE_TEXT);
    int cy = y0 + 22;

    if (cat == (int)Section::Settings) {
        if (tab == 0) { // Customize
            if (customizeOpen == 0) drawLedScreen(x0, cy, w, h - 22);
            else if (customizeOpen == 1) drawDisplayScreen(x0, cy, w, h - 22);
            else if (customizeOpen == 2) drawThemeScreen(x0, cy, w, h - 22);
            else {
                MwDraw::card(canvas, x0, cy, w, h - 22, "CUSTOMIZE");
                // large text like the main menu (used to be small Font0 1x)
                canvas.setFont(&fonts::Font0);
                canvas.setTextSize(Theme::SIZE_MENU);
                canvas.setTextDatum(textdatum_t::middle_left);
                static const char* const ITEMS[3] = {"LED", "Display", "Theme"};
                for (int i = 0; i < 3; i++) {
                    int y = cy + 32 + i * Theme::ROW_MENU;
                    bool sel = fieldIndex[cat] == i;
                    if (sel) { canvas.setTextColor(Theme::ACCENT, Theme::BG); canvas.drawString(">", x0, y); }
                    canvas.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
                    canvas.drawString(ITEMS[i], x0 + 24, y);
                }
                canvas.setTextSize(Theme::SIZE_TEXT);
            }
        }
        else if (tab == 1) drawTimeScreen(x0, cy, w, h - 22);
        else drawAboutScreen(x0, cy, w, h - 22);
    } else if (cat == (int)Section::Wifi) {
        drawWifiScreen(x0, cy, w, h - 22, tab);
    } else if (cat == (int)Section::Files) {
        drawFilesScreen(x0, cy, w, h - 22);
    }
}

void Ui::drawLedScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Settings;
    int sel = fieldIndex[cat];
    const char* modeNames[5] = {"Off", "Solid", "Breathing", "Tail", "Rainbow"};
    char br[8], r[8], g[8], b[8], sp[8];
    snprintf(br, sizeof(br), "%d%%", (int)(ledBrightness * 100 / 255));
    snprintf(r, sizeof(r), "%d", ledR);
    snprintf(g, sizeof(g), "%d", ledG);
    snprintf(b, sizeof(b), "%d", ledB);
    snprintf(sp, sizeof(sp), "%d%%", ledSpeedPct);
    // rows are in sync with input (customizeOpen == 0): 6/7 — toggles,
    // values are pulled straight from Board (settable from WebUI too)
    const char* labels[8] = {"Brightness", "R", "G", "B", "Mode", "Speed", "Click blip", "Rotate glow"};
    const char* values[8] = {br, r, g, b, modeNames[ledModeIdx], sp,
                             Board::getLedClickBlip() ? "on" : "off",
                             Board::getLedRotGlow() ? "on" : "off"};

    int visible = h / Theme::ROW_LIST;
    int top = scrollWindow(0x3000, sel, visible, 8);
    for (int i = 0; i < visible && top + i < 8; i++) {
        int idx = top + i;
        int y = y0 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
        drawListRow(canvas, x0, y, w, idx == sel, idx == sel && ledEditingRow,
                    labels[idx], values[idx], Theme::TEXT);
    }

    // small swatch of the current color at the top right
    canvas.fillRect(x0 + w - 14, y0, 14, 14, RGB565(ledR, ledG, ledB));
    canvas.drawRect(x0 + w - 14, y0, 14, 14, Theme::TEXT_DIM);
}

void Ui::drawDisplayScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Settings;
    int sel = fieldIndex[cat];
    char br[8], dim[16];
    snprintf(br, sizeof(br), "%d%%", (int)(displayBrightness * 100 / 255));
    if (displayAutoDimSec == 0) snprintf(dim, sizeof(dim), "off");
    else snprintf(dim, sizeof(dim), "%ds", displayAutoDimSec);
    // rows are in sync with input (customizeOpen == 1): Brightness -> brightness,
    // Auto-dim -> auto-dim timeout, Rotation -> screen orientation
    const char* labels[3] = {"Brightness", "Auto-dim", "Rotation"};
    const char* values[3] = {br, dim, displayFlipped ? "Flipped" : "Normal"};

    int visible = h / Theme::ROW_LIST;
    int top = scrollWindow(0x3001, sel, visible, 3);
    for (int i = 0; i < visible && top + i < 3; i++) {
        int idx = top + i;
        int y = y0 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
        drawListRow(canvas, x0, y, w, idx == sel, idx == sel && displayEditingRow,
                    labels[idx], values[idx], Theme::TEXT);
    }
}

void Ui::drawTimeScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Settings;
    char buf[12];
    snprintf(buf, sizeof(buf), "UTC%+d", utcOffsetHours);
    drawListRow(canvas, x0, y0 + Theme::ROW_LIST / 2, w, true, true, "UTC offset", buf, Theme::TEXT);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString(WifiMgr::isConnected() ? "synced via NTP" : "connect Wi-Fi to sync",
                      x0 + 12, y0 + Theme::ROW_LIST * 2 + Theme::ROW_LIST / 2);
    (void)h;
}

void Ui::drawThemeScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Settings;
    int sel = fieldIndex[cat];
    char accentBuf[24];
    snprintf(accentBuf, sizeof(accentBuf), "%s", Theme::accentName(themeAccentIdx));
    drawListRow(canvas, x0, y0 + Theme::ROW_LIST / 2, w, sel == 0, false,
                "Accent color", accentBuf, Theme::ACCENT);
    drawListRow(canvas, x0, y0 + Theme::ROW_LIST * 3 / 2, w, sel == 1, false,
                "Footer hints", footerHints ? "on" : "off",
                footerHints ? Theme::TEXT : Theme::TEXT_DIM);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    canvas.drawString("click: change  (saved to NVS)", x0 + 12, y0 + Theme::ROW_LIST * 3);
    (void)h;
}

// ---- About: flat scrollable "key: value" list ----
namespace {
    // Internet check: lightweight HTTP 204 (runs in tick on a flag set
    // by the NETWORK page — drawing is not blocked)
    int netCheckState = -1;
    uint32_t netCheckAt = 0;
    bool netCheckRun = false;
    void netCheckTick() {
        if (!netCheckRun || !WifiMgr::isConnected()) return;
        netCheckRun = false;
        HTTPClient http;
        WiFiClient client;
        http.begin(client, "http://connectivitycheck.gstatic.com/generate_204");
        http.setTimeout(2500);
        int code = http.GET();
        http.end();
        netCheckState = (code == 204) ? 1 : 0;
        netCheckAt = millis();
        Features::requestRedraw();
    }

    // PN532 via Nfc::ensure() (RST pulse + init — not a bare probe)
    bool pn532Probe() {
        return Nfc::ensure();
    }
}

int Ui::aboutRowCount() const { return 35; }

void Ui::drawAboutScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Settings;
    int top = fieldIndex[cat]; // scroll offset (see handleInput)
    if (top < 0) top = 0;
    int visible = (h - 20) / Theme::ROW_LIST;
    if (visible < 1) visible = 1;
    int maxTop = aboutRowCount() - visible;
    if (top > maxTop) top = maxTop;

    uint32_t s = millis() / 1000;
    char v1[24], v2[32], v3[32];
    static char line[48];

    for (int i = 0; i < visible; i++) {
        int row = top + i;
        if (row >= aboutRowCount()) break;
        int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;

        switch (row) {
            case 0:  drawKV(x0, y, w, "", "CHIP", Theme::ACCENT); break;
            case 1:  drawKV(x0, y, w, "Model", ESP.getChipModel(), Theme::TEXT); break;
            case 2:
                snprintf(v1, sizeof(v1), "%d", ESP.getChipCores());
                drawKV(x0, y, w, "Cores", v1, Theme::TEXT); break;
            case 3:
                snprintf(v1, sizeof(v1), "%d MHz", ESP.getCpuFreqMHz());
                drawKV(x0, y, w, "CPU", v1, Theme::TEXT); break;
            case 4:
                snprintf(v1, sizeof(v1), "%d", ESP.getChipRevision());
                drawKV(x0, y, w, "Revision", v1, Theme::TEXT); break;
            case 5:  drawKV(x0, y, w, "", "MEMORY", Theme::ACCENT); break;
            case 6:
                snprintf(v1, sizeof(v1), "%lum %lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
                drawKV(x0, y, w, "Uptime", v1, Theme::TEXT); break;
            case 7:
                snprintf(v1, sizeof(v1), "%lu / %lu KB", (unsigned long)(ESP.getFreeHeap() / 1024),
                         (unsigned long)(ESP.getHeapSize() / 1024));
                drawKV(x0, y, w, "Heap free", v1, Theme::TEXT); break;
            case 8:
                snprintf(v1, sizeof(v1), "%lu / %lu KB", (unsigned long)(ESP.getFreePsram() / 1024),
                         (unsigned long)(ESP.getPsramSize() / 1024));
                drawKV(x0, y, w, "PSRAM", v1, Theme::TEXT); break;
            case 9:
                snprintf(v1, sizeof(v1), "%lu MB", (unsigned long)(ESP.getFlashChipSize() / 1024 / 1024));
                drawKV(x0, y, w, "Flash", v1, Theme::TEXT); break;
            case 10:
                if (Files::sdPresent()) {
                    uint64_t tot = Files::sdTotalBytes(), used = Files::sdUsedBytes();
                    snprintf(v1, sizeof(v1), "%llu / %llu MB", (unsigned long long)(used / 1024 / 1024),
                             (unsigned long long)(tot / 1024 / 1024));
                } else {
                    snprintf(v1, sizeof(v1), "not present");
                }
                drawKV(x0, y, w, "SD", v1, Files::sdPresent() ? Theme::TEXT : Theme::TEXT_DIM); break;
            case 11: drawKV(x0, y, w, "", "PINS", Theme::ACCENT); break;
            case 12: drawKV(x0, y, w, "Encoder", "4 / 5 / 0", Theme::TEXT); break;
            case 13: drawKV(x0, y, w, "Keys", "Quit 6 / PWR 15", Theme::TEXT); break;
            case 14: drawKV(x0, y, w, "CC1101", "CS 12  GDO 3/38", Theme::TEXT); break;
            case 15: drawKV(x0, y, w, "SD / LED", "13 / 14", Theme::TEXT); break;
            case 16: drawKV(x0, y, w, "I2C", "8 / 18", Theme::TEXT); break;
            case 17: drawKV(x0, y, w, "IR", "TX 2 / RX 1", Theme::TEXT); break;
            case 18: drawKV(x0, y, w, "Mic/Spk", "42/39  7/40/46", Theme::TEXT); break;
            case 19: drawKV(x0, y, w, "PN532/NRF", "RST45 IRQ17 / CE43 CS44", Theme::TEXT); break;
            case 20: drawKV(x0, y, w, "", "MODULES", Theme::ACCENT); break;
            case 21:
                snprintf(v2, sizeof(v2), "%s", SubGhz::isReady() ? "OK" : "not found");
                drawKV(x0, y, w, "CC1101", v2, SubGhz::isReady() ? Theme::OK : Theme::TEXT_DIM); break;
            case 22:
                snprintf(v2, sizeof(v2), "%s", Files::sdPresent() ? "mounted" : "absent");
                drawKV(x0, y, w, "SD card", v2, Files::sdPresent() ? Theme::OK : Theme::TEXT_DIM); break;
            case 23:
                snprintf(v2, sizeof(v2), "%s", Files::flashPresent() ? "mounted" : "error");
                drawKV(x0, y, w, "LittleFS", v2, Files::flashPresent() ? Theme::OK : Theme::ERR); break;
            case 24:
                snprintf(v2, sizeof(v2), "%s", Battery::isPresent() ? "OK" : "not found");
                drawKV(x0, y, w, "BQ27220", v2, Battery::isPresent() ? Theme::OK : Theme::TEXT_DIM); break;
            case 25:
                snprintf(v2, sizeof(v2), "%s", pn532Probe() ? "detected" : "not found");
                drawKV(x0, y, w, "PN532", v2, pn532Probe() ? Theme::OK : Theme::TEXT_DIM); break;
            case 26: drawKV(x0, y, w, "BLE", "available", Theme::TEXT_DIM); break;
            case 27: drawKV(x0, y, w, "", "FIRMWARE", Theme::ACCENT); break;
            case 28:
                snprintf(v3, sizeof(v3), "%s v%s", MW_FW_NAME, MW_FW_VERSION);
                drawKV(x0, y, w, "Version", v3, Theme::TEXT); break;
            case 29:
                snprintf(v3, sizeof(v3), "%s %s", __DATE__, __TIME__);
                drawKV(x0, y, w, "Built", v3, Theme::TEXT); break;
            case 30: drawKV(x0, y, w, "Board", "T-Embed CC1101", Theme::TEXT); break;
            case 31: drawKV(x0, y, w, "", "NETWORK", Theme::ACCENT); break;
            case 32:
                if (WifiMgr::isConnected()) {
                    translitToBuf(WifiMgr::currentSSID(), line, sizeof(line));
                    drawKV(x0, y, w, "SSID", line, Theme::TEXT);
                } else {
                    drawKV(x0, y, w, "SSID", "not connected", Theme::TEXT_DIM);
                }
                break;
            case 33:
                if (WifiMgr::isConnected()) {
                    snprintf(line, sizeof(line), "%s", WiFi.localIP().toString().c_str());
                    drawKV(x0, y, w, "IP", line, Theme::TEXT);
                } else {
                    snprintf(line, sizeof(line), "%s", WebUI::isRunning() ? "running :80" : "off");
                    drawKV(x0, y, w, "WebUI", line, WebUI::isRunning() ? Theme::OK : Theme::TEXT_DIM);
                }
                break;
            case 34:
                if (!WifiMgr::isConnected()) {
                    drawKV(x0, y, w, "Internet", "n/a (offline)", Theme::TEXT_DIM);
                } else {
                    if (netState < 0 || millis() - netAt > 30000) netPending = true;
                    const char* sv = netState < 0 ? "checking..." : (netState ? "yes" : "no");
                    drawKV(x0, y, w, "Internet", sv, netState == 1 ? Theme::OK : Theme::TEXT_DIM);
                }
                break;
        }
    }
    drawScrollbar(Theme::SCREEN_W - Theme::SCROLLBAR_W - 1, y0 + 20, visible * Theme::ROW_LIST,
                  top, visible, aboutRowCount());
}

// ---- Wi-Fi: Connect / WebUI / Network ----
void Ui::drawWifiScreen(int x0, int y0, int w, int h, int tab) {
    int cat = (int)Section::Wifi;
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_left);

    if (tab == 0) { // Connect
        int listTop = y0;
        // status
        static char sbuf[64];
        if (WifiMgr::isConnected()) {
            char tbuf[48];
            translitToBuf(WifiMgr::currentSSID(), tbuf, sizeof(tbuf)); // no String: drawn on every state change
            snprintf(sbuf, sizeof(sbuf), "Wi-Fi: %s", tbuf);
            canvas.setTextColor(Theme::OK, Theme::BG);
            canvas.drawString(sbuf, x0 + 12, listTop + 6);
        } else if (WifiMgr::connState() == WifiConnState::Connecting) {
            canvas.setTextColor(Theme::WARN, Theme::BG);
            canvas.drawString("Connecting...", x0 + 12, listTop + 6);
        } else if (WifiMgr::connState() == WifiConnState::Failed) {
            canvas.setTextColor(Theme::ERR, Theme::BG);
            canvas.drawString("Connection failed", x0 + 12, listTop + 6);
        } else {
            canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
            canvas.drawString("Wi-Fi: off", x0 + 12, listTop + 6);
        }
        listTop += Theme::ROW_LIST + 4;

        if (WifiMgr::isScanning()) {
            canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
            canvas.drawString("Scanning...", x0 + 12, listTop + 6);
            return;
        }
        int n = WifiMgr::resultCount();
        if (n == 0) {
            canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
            canvas.drawString("No networks found", x0 + 12, listTop + 6);
            return;
        }
        int visible = (h - (listTop - y0)) / Theme::ROW_LIST;
        if (visible < 1) visible = 1;
        int sel = fieldIndex[cat];
        int top = scrollWindow(0x4000, sel, visible, n);
        for (int i = 0; i < visible && top + i < n; i++) {
            int idx = top + i;
            WifiNetwork net = WifiMgr::result(idx);
            bool s = (idx == sel);
            int y = listTop + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            // no String: translit straight into the row buffer (the frame is drawn often)
            char label[52];
            size_t pos = (size_t)snprintf(label, sizeof(label), "%c ", net.secure ? '*' : ' ');
            translitToBuf(net.ssid, label + pos, sizeof(label) - pos);
            char meta[24];
            snprintf(meta, sizeof(meta), "%lddB ch%d", (long)net.rssi, net.channel);
            drawListRow(canvas, x0, y, w, s, false, label, meta, Theme::TEXT_DIM);
        }
        drawScrollbar(Theme::SCREEN_W - Theme::SCROLLBAR_W - 1, listTop, visible * Theme::ROW_LIST,
                      top, visible, n);
    }
}

// ---- Files: SD / Flash browser ----
void Ui::drawFilesScreen(int x0, int y0, int w, int h) {
    int cat = (int)Section::Files;
    int tab = subTabIndex[cat];
    if (tab == 2) { // WebUI — status + toggle
        MwDraw::card(canvas, x0, y0, w, h, "WEBUI");
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(Theme::SIZE_MENU);
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.setTextColor(webUiEnabled ? Theme::OK : Theme::TEXT_DIM, Theme::BG);
        canvas.drawString(webUiEnabled ? "ON" : "OFF", x0 + 24, y0 + 44);
        canvas.setTextSize(Theme::SIZE_TEXT);
        canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
        if (webUiEnabled) {
            if (WifiMgr::isConnected()) {
                char url[40];
                snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());
                canvas.drawString(url, x0 + 12, y0 + 62);
            } else if (WifiMgr::isAPRunning()) {
                canvas.drawString("AP: 192.168.4.1", x0 + 12, y0 + 62);
            }
        }
        canvas.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        canvas.drawString("click: on/off", x0 + 12, y0 + h - 14);
        return;
    }
    FsRoot root = tab == 0 ? FsRoot::SD : FsRoot::Flash;
    String& path = filesPath[tab];
    bool present = tab == 0 ? Files::sdPresent() : Files::flashPresent();

    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(textdatum_t::middle_left);

    // path
    static char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "%s%s", tab == 0 ? "SD:" : "FL:", path.c_str());
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString(pbuf, x0 + 12, y0 + 6);
    int listTop = y0 + Theme::ROW_LIST + 4;

    if (!present) {
        canvas.setTextColor(Theme::WARN, Theme::BG);
        canvas.drawString(tab == 0 ? "SD card not found" : "Flash FS not mounted", x0 + 12, listTop + 6);
        return;
    }

    int n = Files::listCount(root, path);
    int visible = (h - (listTop - y0) - Theme::ROW_LIST) / Theme::ROW_LIST;
    if (visible < 1) visible = 1;
    int sel = fieldIndex[cat];
    int top = scrollWindow(0x5000 + tab, sel, visible, n);

    for (int i = 0; i < visible && top + i < n; i++) {
        int idx = top + i;
        FsEntry e = Files::listEntry(root, path, idx);
        bool s = (idx == sel);
        int y = listTop + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
        char label[72];
        snprintf(label, sizeof(label), "%s%s", e.name, e.isDir ? "/" : "");
        drawListRow(canvas, x0, y, w, s, false, label, "", Theme::TEXT);
    }
    if (n == 0) {
        canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
        canvas.drawString("(empty)", x0 + 12, listTop + 6);
    }
    drawScrollbar(Theme::SCREEN_W - Theme::SCROLLBAR_W - 1, listTop, visible * Theme::ROW_LIST,
                  top, visible, n);

    if (playing) {
        char line[56];
        snprintf(line, sizeof(line), "▶ %s  %lu/%lu KB", playTitle,
                 (unsigned long)(playPos / 1024), (unsigned long)(playTotal / 1024));
        canvas.setTextColor(Theme::ACCENT, Theme::BG);
        canvas.drawString(line, x0 + 12, y0 + h - 14);
    } else if (lastScriptResult.length() && millis() - lastScriptResultAt < 4000) {
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.setTextColor(lastScriptResult.startsWith("OK") ? Theme::OK : Theme::ERR, Theme::BG);
        canvas.drawString(lastScriptResult, x0 + 12, y0 + h - 14);
    }
}

// =====================================================================
//  Modal: File Reader — book view of .txt / .pdf opened from Files.
//  TXT: UTF-8 or CP1251 (auto), Cyrillic transliterated to the display
//  font. PDF: text harvested from content streams (FlateDecode streams
//  are inflated with the ROM miniz) - Tj/TJ/'/"/<hex> strings, line
//  breaks on Td/TD/T*/ET. Good enough for documentation PDFs; scanned
//  or CID-font PDFs honestly report "no text extracted".
//  Everything lives in PSRAM (text + line table), freed on close.
//  Navigation: rotation = line scroll, click = page, hold = page back,
//  Quit = exit. The position is bookmarked in NVS per file path.
// =====================================================================
namespace {
    constexpr size_t RD_TEXT_CAP = 256 * 1024;
    constexpr uint32_t RD_MAX_LINES = 16384;
    constexpr int RD_COLS = 50;      // Font0 chars per body line
    constexpr int RD_ROWS = 12;      // body lines per screen

    bool rdIsUtf8(const uint8_t* p, size_t n) {
        size_t i = 0;
        while (i < n) {
            uint8_t c = p[i];
            if (c < 0x80) { i++; continue; }
            if ((c & 0xE0) == 0xC0) { if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return false; i += 2; continue; }
            if ((c & 0xF0) == 0xE0) { if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false; i += 3; continue; }
            if ((c & 0xF8) == 0xF0) { if (i + 3 >= n) return false; i += 4; continue; }
            return false; // 0x80-0xBF or 0xFE/0xFF outside a sequence
        }
        return true;
    }

    // CP1251 -> UTF-8 (Cyrillic + quotes); ASCII passes through
    size_t rdCp1251ToUtf8(const uint8_t* in, size_t n, char* out, size_t cap) {
        size_t o = 0;
        for (size_t i = 0; i < n && o + 2 < cap; i++) {
            uint8_t b = in[i];
            uint16_t cp = 0;
            if (b < 0x80) { out[o++] = (char)b; continue; }
            if (b == 0xA8) cp = 0x0401;
            else if (b == 0xB8) cp = 0x0451;
            else if (b >= 0xC0) cp = 0x0410 + (b - 0xC0);
            if (!cp) { out[o++] = '?'; continue; }
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
        return o;
    }

    uint32_t rdPathHash(const char* s) {
        uint32_t h = 2166136261u;
        while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
        return h;
    }

    // zlib (PDF FlateDecode) -> dst via the ROM tinfl; false if over
    // capacity or broken
    bool rdInflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t& outLen) {
        size_t r = tinfl_decompress_mem_to_mem(dst, dstCap, src, srcLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
        outLen = r;
        return true;
    }

    // harvest text tokens from a PDF content stream
    void rdHarvestPdfText(const uint8_t* c, size_t n, char* out, size_t cap, size_t& o) {
        for (size_t i = 0; i < n;) {
            uint8_t ch = c[i];
            if (ch == '(') { // literal string with escapes and nesting
                i++;
                int depth = 1;
                while (i < n && o + 1 < cap) {
                    uint8_t s = c[i++];
                    if (s == '\\') {
                        if (i >= n) break;
                        uint8_t e = c[i++];
                        if (e == 'n') out[o++] = '\n';
                        else if (e == 't') out[o++] = ' ';
                        else if (e == '(' || e == ')' || e == '\\') out[o++] = (char)e;
                        else if (e >= '0' && e <= '7') {
                            int v = e - '0';
                            int k = 1;
                            while (k < 3 && i < n && c[i] >= '0' && c[i] <= '7') { v = v * 8 + (c[i] - '0'); i++; k++; }
                            if (v >= 32 && v < 127) out[o++] = (char)v;
                            else if (v == 10) out[o++] = '\n';
                        }
                        continue;
                    }
                    if (s == '(') { depth++; out[o++] = '('; continue; }
                    if (s == ')') { depth--; if (depth == 0) break; out[o++] = ')'; continue; }
                    out[o++] = (char)s;
                }
                if (o < cap) out[o++] = ' ';
                continue;
            }
            if (ch == '<' && i + 1 < n && isxdigit((int)c[i + 1]) && c[i + 1] != '<') { // hex string
                i++;
                bool pending = false;
                uint8_t hi = 0;
                while (i < n && o + 1 < cap) {
                    uint8_t h = c[i];
                    if (h == '>') { i++; break; }
                    int v;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    else { i++; continue; }
                    i++;
                    if (!pending) { hi = (uint8_t)(v << 4); pending = true; }
                    else {
                        uint8_t b = hi | v;
                        pending = false;
                        if (b >= 32 && b < 127) out[o++] = (char)b;
                    }
                }
                if (o < cap) out[o++] = ' ';
                continue;
            }
            if (ch == 'T' && i + 1 < n && (c[i + 1] == 'd' || c[i + 1] == 'D' || c[i + 1] == '*')) {
                if (o && out[o - 1] != '\n' && o + 1 < cap) out[o++] = '\n';
                i += 2; continue;
            }
            if (ch == 'E' && i + 1 < n && c[i + 1] == 'T') {
                if (o && out[o - 1] != '\n' && o + 1 < cap) out[o++] = '\n';
                i += 2; continue;
            }
            if (ch == 'B' && i + 1 < n && c[i + 1] == 'T') {
                if (o && out[o - 1] != '\n' && o + 1 < cap) out[o++] = '\n';
                i += 2; continue;
            }
            i++;
        }
        if (o < cap) out[o] = 0;
    }
}

void Ui::openReader(FsRoot root, const String& path) {
    closeReader();
    strncpy(readerPath, path.c_str(), sizeof(readerPath) - 1);
    readerPath[sizeof(readerPath) - 1] = 0;
    String lower = path; lower.toLowerCase();
    readerPdf = lower.endsWith(".pdf");

    if (!psramFound()) { modalKind = ModalKind::None; return; }
    readerText = (char*)ps_malloc(RD_TEXT_CAP);
    if (!readerText) { modalKind = ModalKind::None; return; }
    readerText[0] = 0;
    readerTextLen = 0;

    fs::FS& fsRef = root == FsRoot::SD ? (fs::FS&)SD : (fs::FS&)LittleFS;

    // ---- PDF: walk streams, inflate FlateDecode, harvest text ----
    if (readerPdf) {
        File f = fsRef.open(readerPath);
        if (!f) { free(readerText); readerText = nullptr; return; }
        uint8_t* scan = (uint8_t*)ps_malloc(32768);     // sliding scan window
        uint8_t* streamBuf = (uint8_t*)ps_malloc(384 * 1024); // raw stream data
        static uint8_t* infl = nullptr;                 // inflated content (cache)
        static char* seg = nullptr;                     // harvest segment (cache)
        if (!scan || !streamBuf) {
            if (scan) free(scan);
            if (streamBuf) free(streamBuf);
            f.close();
            free(readerText); readerText = nullptr;
            return;
        }

        // static "PARSING" screen
        canvas.fillSprite(Theme::BG);
        canvas.setTextColor(Theme::ACCENT, Theme::BG);
        canvas.drawString("PARSING PDF...", 10, 20);
        canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
        canvas.drawString(readerPath, 10, 36);
        canvas.pushSprite(0, Y_OFFSET);

        uint32_t fileLen = f.size();
        uint32_t scanPos = 0;      // file offset of scan[0]
        size_t scanFill = 0;       // valid bytes in scan[]
        bool eof = false;
        int streams = 0;

        auto drawProgress = [&]() {
            canvas.fillRect(0, 50, Theme::SCREEN_W, 16, Theme::BG);
            char line[48];
            snprintf(line, sizeof(line), "streams: %d   text: %lu KB", streams,
                     (unsigned long)(readerTextLen / 1024));
            canvas.setTextColor(Theme::TEXT, Theme::BG);
            canvas.drawString(line, 10, 52);
            canvas.pushSprite(0, Y_OFFSET);
        };

        while (readerTextLen < RD_TEXT_CAP - 4096 && !eof) {
            // top up the scan window
            if (scanFill < 32768 && f.position() < (long)fileLen) {
                size_t want = 32768 - scanFill;
                if ((uint32_t)(f.position() + want) > fileLen) want = fileLen - f.position();
                size_t got = f.read(scan + scanFill, want);
                scanFill += got;
                if (f.position() >= (long)fileLen) eof = true;
            } else if (scanFill < 8) break;

            // find "stream" (not "endstream")
            int hit = -1;
            for (size_t i = 8; i + 6 <= scanFill; i++) {
                if (memcmp(scan + i, "stream", 6) == 0 &&
                    !(i >= 3 && memcmp(scan + i - 3, "end", 3) == 0)) { hit = (int)i; break; }
            }
            if (hit < 0) {
                // keep a small tail for boundary crossings
                size_t keep = 64;
                if (scanFill > keep) {
                    memmove(scan, scan + scanFill - keep, keep);
                    scanPos += scanFill - keep;
                    scanFill = keep;
                } else if (eof) break;
                continue;
            }

            // dict before the keyword
            size_t dictStart = hit > 320 ? hit - 320 : 0;
            size_t dictLen = hit - dictStart;
            bool flate = false, skip = false;
            for (size_t i = dictStart; i + 12 < dictStart + dictLen; i++) {
                if (memcmp(scan + i, "FlateDecode", 11) == 0) flate = true;
                if (memcmp(scan + i, "/Image", 6) == 0) skip = true;
                if (memcmp(scan + i, "Length1", 7) == 0) skip = true; // embedded font
            }

            // data start after "stream" + EOL
            long dataFilePos = (long)(scanPos + hit + 6);
            if (hit + 6 < (int)scanFill && scan[hit + 6] == '\r') dataFilePos++;
            if (hit + 6 < (int)scanFill && scan[hit + 6 + ((scan[hit + 6] == '\r') ? 1 : 0)] == '\n') dataFilePos++;

            // find "endstream" forward via seeks
            long endPos = -1;
            uint32_t streamLen = 0;
            f.seek(dataFilePos);
            uint8_t endBuf[512];
            long p = dataFilePos;
            while (p < (long)fileLen && streamLen < 384 * 1024) {
                size_t got = f.read(endBuf, sizeof(endBuf));
                if (!got) break;
                for (size_t i = 0; i + 9 <= got; i++) {
                    if (memcmp(endBuf + i, "endstream", 9) == 0) { endPos = p + (long)i; break; }
                }
                if (endPos >= 0) break;
                p += got;
                streamLen += got;
            }
            if (endPos < 0) { // malformed tail
                scanPos = fileLen; scanFill = 0; break;
            }
            streamLen = (uint32_t)(endPos - dataFilePos);
            if (streamLen > 384 * 1024) streamLen = 384 * 1024;

            // read the raw stream
            f.seek(dataFilePos);
            size_t rawLen = f.read(streamBuf, streamLen);

            if (!skip && rawLen > 0) {
                const uint8_t* content = streamBuf;
                size_t contentLen = rawLen;
                if (flate) {
                    if (!infl) infl = (uint8_t*)ps_malloc(192 * 1024);
                    size_t inflLen = 0;
                    if (infl && rdInflate(streamBuf, rawLen, infl, 192 * 1024, inflLen)) {
                        content = infl;
                        contentLen = inflLen;
                    } else { content = nullptr; }
                }
                if (content) {
                    // harvest into a temp segment, keep if mostly printable
                    if (!seg) seg = (char*)ps_malloc(64 * 1024);
                    if (seg) {
                        size_t so = 0;
                        seg[0] = 0;
                        rdHarvestPdfText(content, contentLen, seg, 64 * 1024 - 1, so);
                        uint32_t printable = 0;
                        for (size_t i = 0; i < so; i++) {
                            uint8_t ch = (uint8_t)seg[i];
                            if (ch == '\n' || ch == ' ' || (ch >= 0x20 && ch < 0x7F)) printable++;
                        }
                        if (so > 16 && printable * 100 > so * 60 && readerTextLen + so < RD_TEXT_CAP - 2048) {
                            memcpy(readerText + readerTextLen, seg, so);
                            readerTextLen += so;
                            if (readerText[readerTextLen - 1] != '\n' && readerTextLen < RD_TEXT_CAP - 2)
                                readerText[readerTextLen++] = '\n';
                        }
                    }
                }
            }
            streams++;
            if (streams % 4 == 0) drawProgress();

            // resume scanning after endstream
            long next = endPos + 9;
            f.seek(next);
            scanPos = (uint32_t)next;
            scanFill = 0;
            eof = f.position() >= (long)fileLen;
        }
        if (infl) { free(infl); infl = nullptr; }
        if (seg) { free(seg); seg = nullptr; }
        free(streamBuf);
        free(scan);
        f.close();
        readerText[readerTextLen] = 0;
        if (readerTextLen < 32) { // nothing usable (scanned / CID-font PDF)
            const char* msg = "No text extracted (scanned or CID-font PDF?)";
            strncpy(readerText, msg, RD_TEXT_CAP - 1);
            readerTextLen = strlen(msg);
        } else if (readerTextLen >= RD_TEXT_CAP - 2048) {
            const char* note = "\n...(truncated at 256 KB)";
            size_t nl = strlen(note);
            if (readerTextLen + nl < RD_TEXT_CAP - 1) {
                memcpy(readerText + readerTextLen, note, nl);
                readerTextLen += nl;
            }
        }
    } else {
        // ---- TXT: UTF-8 or CP1251, transliterated to the display font ----
        File f = fsRef.open(readerPath);
        if (!f) { free(readerText); readerText = nullptr; return; }
        static char* conv = nullptr; // chunk conversion scratch (SPIRAM)
        if (!conv) conv = (char*)ps_malloc(16384);
        if (!conv) { f.close(); free(readerText); readerText = nullptr; return; }

        uint8_t chunk[4096];
        bool first = true;
        bool isUtf8 = true;
        while (readerTextLen < RD_TEXT_CAP - 8192) {
            size_t got = f.read(chunk, sizeof(chunk));
            if (!got) break;
            if (first) { isUtf8 = rdIsUtf8(chunk, got); first = false; readerCp1251 = !isUtf8; }
            size_t add = 0;
            if (isUtf8) {
                String s;
                s.reserve(got + 1);
                s.concat((const char*)chunk, got);
                translitToBuf(s, conv, 16383);
                add = strlen(conv);
            } else {
                size_t u8 = rdCp1251ToUtf8(chunk, got, conv, 16383);
                conv[u8] = 0;
                String s; s.reserve(u8 + 1); s.concat(conv, u8);
                translitToBuf(s, conv, 16383);
                add = strlen(conv);
            }
            if (readerTextLen + add >= RD_TEXT_CAP - 8) break;
            memcpy(readerText + readerTextLen, conv, add);
            readerTextLen += add;
        }
        conv[0] = 0;
        readerText[readerTextLen] = 0;
        f.close();
        if (readerTextLen >= RD_TEXT_CAP - 8192) {
            const char* note = "\n...(file truncated at 256 KB)";
            size_t nl = strlen(note);
            memcpy(readerText + readerTextLen, note, nl + 1);
            readerTextLen += nl;
        }
    }

    // ---- wrap into display lines ----
    readerLines = (uint32_t*)ps_malloc(RD_MAX_LINES * sizeof(uint32_t));
    if (!readerLines) { free(readerText); readerText = nullptr; return; }
    readerLineCount = 0;
    {
        uint32_t pos = 0;
        while (pos < readerTextLen && readerLineCount < RD_MAX_LINES) {
            readerLines[readerLineCount++] = pos;
            uint32_t col = 0, brk = 0;
            bool sawSpace = false;
            while (pos < readerTextLen) {
                char ch = readerText[pos];
                if (ch == '\n') { pos++; break; }
                if (col >= RD_COLS) {
                    // wrap at the last space so words stay whole
                    if (brk > readerLines[readerLineCount - 1] && sawSpace) pos = brk;
                    break;
                }
                if (ch == ' ') { sawSpace = true; brk = pos; }
                pos++;
                col++;
            }
        }
    }
    if (readerLineCount == 0) { closeReader(); return; }

    // ---- bookmark restore (NVS, per path) ----
    char key[16];
    snprintf(key, sizeof(key), "bm_%08X", (unsigned)rdPathHash(readerPath));
    int saved = Config::getInt(key, 0);
    readerCur = (saved > 0 && (uint32_t)saved < readerLineCount) ? (uint32_t)saved : 0;

    modalKind = ModalKind::Reader;
    focusLevel = FocusLevel::Modal;
    frameMoving = true;
    lastFrameAnimated = true;
}

void Ui::closeReader() {
    if (modalKind == ModalKind::Reader && readerPath[0]) {
        // persist the bookmark FIRST (readerCur is zeroed below)
        char key[16];
        snprintf(key, sizeof(key), "bm_%08X", (unsigned)rdPathHash(readerPath));
        Config::setInt(key, (int)readerCur);
    }
    if (readerText) { free(readerText); readerText = nullptr; }
    if (readerLines) { free(readerLines); readerLines = nullptr; }
    readerTextLen = 0;
    readerLineCount = 0;
    readerCur = 0;
    if (modalKind == ModalKind::Reader) {
        modalKind = ModalKind::None;
        focusLevel = FocusLevel::Field;
        frameMoving = true;
    }
    readerPath[0] = 0;
}

void Ui::drawModalReader() {
    canvas.fillSprite(Theme::BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextDatum(textdatum_t::top_left);

    // title: file name (after the last slash), transliterated
    const char* base = strrchr(readerPath, '/');
    base = base ? base + 1 : readerPath;
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    canvas.drawString(base, 8, 2);

    if (!readerText || readerLineCount == 0) return;

    // body lines
    canvas.setTextColor(Theme::TEXT, Theme::BG);
    char line[RD_COLS + 2];
    for (int i = 0; i < RD_ROWS && readerCur + i < readerLineCount; i++) {
        uint32_t start = readerLines[readerCur + i];
        uint32_t end = (readerCur + i + 1 < readerLineCount) ? readerLines[readerCur + i + 1]
                                                             : readerTextLen;
        uint32_t len = end - start;
        if (len && readerText[start + len - 1] == '\n') len--;
        if (len > RD_COLS) len = RD_COLS;
        memcpy(line, readerText + start, len);
        line[len] = 0;
        canvas.drawString(line, 8, 14 + i * 10);
    }

    // progress
    int pct = (int)(readerCur * 100 / (readerLineCount ? readerLineCount - 1 : 1));
    canvas.drawRect(8, Theme::SCREEN_H - 14, Theme::SCREEN_W - 16, 5, Theme::TEXT_DIM);
    canvas.fillRect(8, Theme::SCREEN_H - 14, (Theme::SCREEN_W - 16) * pct / 100, 5, Theme::ACCENT);
    canvas.setTextColor(Theme::TEXT_FAINT, Theme::BG);
    char pb[48];
    snprintf(pb, sizeof(pb), "%lu/%lu  %d%%  %s", (unsigned long)(readerCur + 1),
             (unsigned long)readerLineCount, pct, readerPdf ? "PDF" : (readerCp1251 ? "TXT cp1251" : "TXT"));
    canvas.drawString(pb, 8, Theme::SCREEN_H - 8);

    canvas.setTextColor(Theme::TEXT_FAINT, Theme::BG);
    canvas.setTextDatum(textdatum_t::top_right);
    canvas.drawString("quit: bookmark+exit", Theme::SCREEN_W - 8, Theme::SCREEN_H - 8);
    canvas.setTextDatum(textdatum_t::top_left);
}

// =====================================================================
//  Modal: on-screen QWERTY keyboard (Terminal-Lite)
// =====================================================================
void Ui::drawModalWifiKeyboard() {
    canvas.fillSprite(Theme::BG);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(Theme::SIZE_TEXT);
    canvas.setTextDatum(textdatum_t::top_center);
    canvas.setTextColor(Theme::ACCENT, Theme::BG);
    char ssidTitle[96];
    translitToBuf(modalTargetSSID, ssidTitle, sizeof(ssidTitle)); // no String per redraw
    if (canvas.textWidth(ssidTitle) > 250) {
        // truncate the title part, "..." stays at the end
        size_t len = strlen(ssidTitle);
        if (len > sizeof(ssidTitle) - 4) len = sizeof(ssidTitle) - 4;
        strcpy(ssidTitle + len, "...");
        while (len > 0 && canvas.textWidth(ssidTitle) > 250) {
            memmove(ssidTitle + len - 1, ssidTitle + len, 4); // shift left, keep "..."
            len--;
        }
    }
    canvas.drawString(ssidTitle, Theme::SCREEN_W / 2, 2);
    if (kbCaps) {
        canvas.setTextDatum(textdatum_t::top_right);
        canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
        canvas.drawString("CAPS", Theme::SCREEN_W - Theme::PAD, 2);
        canvas.setTextDatum(textdatum_t::top_center);
    }
    if (kbShowPlain) {
        canvas.setTextDatum(textdatum_t::top_left);
        canvas.setTextColor(Theme::ACCENT, Theme::BG);
        canvas.drawString("TXT", Theme::PAD, 2);
        canvas.setTextDatum(textdatum_t::top_center);
    }

    // password row: mask or plain text (TXT) + character counter
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setTextColor(Theme::TEXT, Theme::BG);
    String shown;
    if (kbShowPlain) shown = pwBuffer;
    else {
        shown.reserve(pwBuffer.length());
        for (uint32_t i = 0; i < pwBuffer.length(); i++) shown += '*';
    }
    if (canvas.textWidth(shown) > 250) {
        shown = "..." + shown; // truncate the content part, "..." stays at the front
        while (shown.length() > 4 && canvas.textWidth(shown) > 250) shown.remove(1, 1);
    }
    canvas.drawString(shown, 26, 25);
    canvas.drawFastHLine(20, 34, 280, Theme::TEXT_DIM);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
    char pwCount[8];
    snprintf(pwCount, sizeof(pwCount), "%u", (unsigned)pwBuffer.length());
    canvas.drawString(pwCount, 296, 25);

    const int* rowLen = kbMode == 0 ? kb::lettersRowLen : kb::symsRowLen;
    const char* const* funcRow = kbMode == 0 ? kb::funcRowLetters : kb::funcRowSyms;
    int totalGridKeys = rowLen[0] + rowLen[1] + rowLen[2];

    int y0 = 40, rowH = 24, cellW = 30, gridX0 = 10;
    canvas.setTextDatum(textdatum_t::middle_center);

    int idx = 0;
    for (int r = 0; r < 3; r++) {
        const char* const* row = kbMode == 0 ? kb::lettersRows[r] : kb::symsRows[r];
        for (int c = 0; c < rowLen[r]; c++) {
            const char* rawLabel = row[c];
            bool sel = (idx == kbIndex);
            bool isShift = (rawLabel[0] == '^');
            int cx = gridX0 + c * cellW;
            int cy = y0 + r * rowH;
            if (sel) {
                // selected key — inverted (accent block, black text)
                canvas.fillRect(cx, cy, cellW - 3, rowH - 4, Theme::ACCENT);
                canvas.setTextColor(Theme::BG, Theme::ACCENT);
            } else {
                canvas.setTextColor(isShift && kbCaps ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
            }
            // per-key label without String: a char + NUL (uppercase in place)
            char lbl[2] = { rawLabel[0], 0 };
            if (kbMode == 0 && kbCaps && lbl[0] && isalpha((int)lbl[0])) lbl[0] = (char)toupper((int)lbl[0]);
            canvas.drawString(lbl, cx + (cellW - 3) / 2, cy + (rowH - 4) / 2);
            idx++;
        }
    }

    int fy = y0 + 3 * rowH;
    int fw = 300 / 6;
    for (int i = 0; i < 6; i++) {
        bool sel = (idx == kbIndex);
        int cx = gridX0 + i * fw;
        if (sel) {
            canvas.fillRect(cx, fy, fw - 3, rowH - 4, Theme::ACCENT);
            canvas.setTextColor(Theme::BG, Theme::ACCENT);
        } else {
            canvas.setTextColor(Theme::TEXT_DIM, Theme::BG);
        }
        canvas.drawString(funcRow[i], cx + (fw - 3) / 2, fy + (rowH - 4) / 2);
        idx++;
    }

    // connection status
    canvas.setTextDatum(textdatum_t::middle_center);
    WifiConnState cs = WifiMgr::connState();
    if (cs == WifiConnState::Connecting) {
        canvas.setTextColor(Theme::WARN, Theme::BG);
        canvas.drawString("Connecting...", Theme::SCREEN_W / 2, 160);
    } else if (cs == WifiConnState::Failed) {
        canvas.setTextColor(Theme::ERR, Theme::BG);
        canvas.drawString("Connection failed", Theme::SCREEN_W / 2, 160);
    } else if (cs == WifiConnState::Connected) {
        canvas.setTextColor(Theme::OK, Theme::BG);
        canvas.drawString("Connected!", Theme::SCREEN_W / 2, 160);
    }
}

// =====================================================================
//  Modal: WebUI settings (AP address) — Terminal-Lite
// =====================================================================
