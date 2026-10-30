// =====================================================================
//  Tools: Logic Analyzer 2.0 — capture + PROTOCOL DECODE on GPIO.
//  Modes:
//    RAW   — v1 behavior: waveform of edges on one pin
//    UART  — single wire decode: baud autodetect, 8N1/8E1 bytes + errors
//    I2C   — 2 wires (SDA/SCL): START/STOP, address+R/W, ACK, data
//    SPI   — 4 wires: CS segments, MOSI/MISO byte pairs on SCK rising
//  Capture: shared edge ISR on all active pins records (time, level
//  bitmask) - no heap, fully static buffers (5.5 KB).
//  Rotation: select row / scroll table. Click: edit / start / back.
//  Long press: save decoded session to /la/logic.csv.
//  Pins are 3.3 V logic only - use a divider for 5 V devices.
// =====================================================================
#include "../ui/features.h"
#include "../../include/pins.h"
#include "../../include/theme.h"
#include <esp_timer.h>
#include <driver/gpio.h>
#include <SD.h>

namespace {
    // ---- capture (static, ISR-safe: no allocation anywhere) ----
    // 1024 events x 5 bytes = 5 KB total. Decoders walk the event
    // stream in place - no intermediate run arrays (RAM discipline).
    constexpr int LA_MAX = 1024;
    static volatile int laCount = 0;
    static volatile uint32_t laTimes[LA_MAX];  // us from start
    static volatile uint8_t laMask[LA_MAX];    // level bitmask of active pins
    static bool capturing = false;
    static int64_t laStartTime = 0;

    enum Mode { M_RAW, M_UART, M_I2C, M_SPI };
    static const char* MODE_NAMES[4] = {"RAW", "UART", "I2C", "SPI"};
    static int mode = M_UART;

    // pin pools per mode
    static const int UART_PINS[] = {17, 43, 44, 1, 2};
    static const int UART_PIN_N = 5;
    // fixed assignments
    constexpr int I2C_SDA = 17, I2C_SCL = 43;
    constexpr int SPI_SCK = 43, SPI_CS = 44, SPI_MOSI = 17, SPI_MISO = 1;

    int uartPinIdx = 0;
    int uiRow = 0;          // 0 mode, 1 pin, 2 start
    bool tableView = false;
    int tableScroll = 0;

    // decoded transaction table (static)
    struct Row {
        uint32_t tUs;
        uint8_t kind;   // mode-specific meaning
        uint8_t val;    // byte / addr<<1|rw / flags
        uint8_t aux;    // second byte (SPI miso) or error flags
    };
    constexpr int MAX_ROWS = 96;
    Row rows[MAX_ROWS];
    int rowCount = 0;
    char modeInfo[40] = ""; // "UART ~9600 8N1 (guessed)" etc
}

// The ISR needs the actual pin numbers, not bit slots; keep a static
// pin table and map mask bit -> gpio via it.
namespace {
    static int laPins[8] = {0};       // mask bit -> gpio
    static int laPinN = 0;

    void IRAM_ATTR laIsrImpl(void* arg) {
        if (laCount >= LA_MAX) return;
        uint8_t m = 0;
        for (int i = 0; i < laPinN; i++)
            if (gpio_get_level((gpio_num_t)laPins[i])) m |= (1 << i);
        if (laCount == 0) laStartTime = esp_timer_get_time();
        laTimes[laCount] = (uint32_t)(esp_timer_get_time() - laStartTime);
        laMask[laCount] = m;
        laCount++;
    }

    void beginCapture() {
        // resolve pins for the current mode
        laPinN = 0;
        if (mode == M_RAW || mode == M_UART) {
            laPins[0] = UART_PINS[uartPinIdx];
            laPinN = 1;
        } else if (mode == M_I2C) {
            laPins[0] = I2C_SDA; laPins[1] = I2C_SCL;
            laPinN = 2;
        } else {
            laPins[0] = SPI_SCK; laPins[1] = SPI_CS;
            laPins[2] = SPI_MOSI; laPins[3] = SPI_MISO;
            laPinN = 4;
        }
        laCount = 0;
        rowCount = 0;
        tableView = false;
        tableScroll = 0;
        modeInfo[0] = 0;
        uint64_t mask = 0;
        for (int i = 0; i < laPinN; i++) mask |= (1ULL << laPins[i]);
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = mask;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&cfg);
        static bool isrServiceInstalled = false;
        if (!isrServiceInstalled) {
            gpio_install_isr_service(0);
            isrServiceInstalled = true;
        }
        for (int i = 0; i < laPinN; i++)
            gpio_isr_handler_add((gpio_num_t)laPins[i], laIsrImpl, nullptr);
        laStartTime = esp_timer_get_time();
        capturing = true;
    }

    void endCapture() {
        for (int i = 0; i < laPinN; i++)
            gpio_isr_handler_remove((gpio_num_t)laPins[i]);
        capturing = false;
    }

    // ---- event stream access helpers ----
    // level of mask bit `bit` at event i (level BEFORE the event is the
    // previous mask; laMask[i] is the state right AFTER event i)
    inline bool evBit(int i, int bit) { return laMask[i] & (1 << bit); }

    void pushRow(uint32_t t, uint8_t kind, uint8_t val, uint8_t aux) {
        if (rowCount >= MAX_ROWS) return;
        rows[rowCount].tUs = t;
        rows[rowCount].kind = kind;
        rows[rowCount].val = val;
        rows[rowCount].aux = aux;
        rowCount++;
    }

    // ---- UART decode: single wire (mask bit 0) ----
    // Walks the event stream directly - no intermediate run arrays.
    void decodeUart() {
        if (laCount < 8) { snprintf(modeInfo, sizeof(modeInfo), "too few edges"); return; }
        // bit period = shortest gap between events above the glitch floor
        uint32_t bitUs = 0;
        for (int i = 1; i < laCount; i++) {
            uint32_t d = laTimes[i] - laTimes[i - 1];
            if (d >= 8 && (!bitUs || d < bitUs)) bitUs = d;
        }
        if (!bitUs) { snprintf(modeInfo, sizeof(modeInfo), "no signal"); return; }
        // snap to standard rates (within 12%)
        static const uint32_t STD[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
        for (unsigned i = 0; i < sizeof(STD) / sizeof(STD[0]); i++) {
            uint32_t want = 1000000UL / STD[i];
            if (bitUs * 100 < want * 112 && want * 100 < bitUs * 112) { bitUs = want; break; }
        }
        snprintf(modeInfo, sizeof(modeInfo), "UART ~%lu baud 8N1 (guessed)",
                 (unsigned long)(1000000UL / bitUs));

        // level at time t: advance a monotonic cursor through the events
        int cursor = 0;
        auto levelAt = [&](uint32_t t) -> bool {
            while (cursor + 1 < laCount && (uint32_t)laTimes[cursor + 1] <= t) cursor++;
            return laMask[cursor] & 1;
        };

        int decoded = 0;
        for (int i = 1; i < laCount && rowCount < MAX_ROWS; i++) {
            bool prev = laMask[i - 1] & 1, now = laMask[i] & 1;
            if (!prev || now) continue; // falling edge = start bit candidate
            uint32_t tStart = laTimes[i];
            uint8_t byte = 0;
            bool ok = true;
            for (int b = 0; b < 8; b++) {
                uint32_t ts = tStart + bitUs * (2 * b + 3) / 2; // 1.5T, 2.5T, ...
                if (ts > (uint32_t)laTimes[laCount - 1]) { ok = false; break; }
                if (levelAt(ts)) byte |= (1 << b);
            }
            bool stopOk = true;
            uint32_t tStop = tStart + bitUs * 10;
            if (tStop <= (uint32_t)laTimes[laCount - 1] && !levelAt(tStop)) stopOk = false;
            if (ok && (stopOk || decoded < 8)) {
                pushRow(tStart, stopOk ? 0 : 3, byte, 0);
                decoded++;
            }
            // skip past this frame (10 bit periods)
            while (i + 1 < laCount && (uint32_t)laTimes[i + 1] < tStop) i++;
        }
        if (!decoded) snprintf(modeInfo, sizeof(modeInfo), "UART: no valid frames (%lu baud?)",
                               (unsigned long)(1000000UL / bitUs));
    }

    // ---- I2C decode: mask bit0 = SDA, bit1 = SCL ----
    void decodeI2c() {
        if (laCount < 8) { snprintf(modeInfo, sizeof(modeInfo), "too few edges"); return; }
        int bits = 0;
        uint8_t byte = 0;
        bool haveAddr = false;
        for (int i = 1; i < laCount && rowCount < MAX_ROWS; i++) {
            bool scl = evBit(i, 1), sda = evBit(i, 0);
            bool pscl = evBit(i - 1, 1), psda = evBit(i - 1, 0);
            if (pscl && scl) { // SCL stayed high: SDA transitions are control
                if (psda && !sda) { // START
                    pushRow(laTimes[i], 0, 0, 0);
                    bits = 0;
                    haveAddr = false;
                } else if (!psda && sda) { // STOP
                    pushRow(laTimes[i], 1, 0, 0);
                    bits = 0;
                    haveAddr = false;
                }
            } else if (!pscl && scl) { // SCL rising: sample SDA
                if (bits < 8) {
                    byte = (byte << 1) | sda;
                    bits++;
                } else { // 9th clock: ACK
                    bool ack = !sda;
                    if (!haveAddr) {
                        pushRow(laTimes[i], 2, byte, ack ? 1 : 0); // addr<<1|rw
                        haveAddr = true;
                    } else {
                        pushRow(laTimes[i], 3, byte, ack ? 1 : 0);
                    }
                    bits = 0;
                    byte = 0;
                }
            }
        }
        snprintf(modeInfo, sizeof(modeInfo), "I2C: %d events decoded", rowCount);
    }

    // ---- SPI decode: bit0=SCK, bit1=CS, bit2=MOSI, bit3=MISO ----
    void decodeSpi() {
        if (laCount < 8) { snprintf(modeInfo, sizeof(modeInfo), "too few edges"); return; }
        int bits = 0;
        uint8_t mosi = 0, miso = 0;
        uint32_t segT = 0;
        bool inSeg = false;
        int segRows = 0;
        for (int i = 1; i < laCount && rowCount < MAX_ROWS; i++) {
            bool sck = evBit(i, 0), cs = evBit(i, 1);
            bool psck = evBit(i - 1, 0), pcs = evBit(i - 1, 1);
            if (pcs && !cs) { // CS falling: segment start
                segT = laTimes[i];
                segRows = rowCount;
                inSeg = true;
                bits = 0;
                mosi = miso = 0;
            } else if (!pcs && cs && inSeg) { // CS rising: segment end
                pushRow(segT, 1, segRows == rowCount ? 0 : 1, rowCount - segRows); // kind1=segment summary
                inSeg = false;
            } else if (inSeg && !psck && sck) { // SCK rising: sample
                mosi = (mosi << 1) | evBit(i, 2);
                miso = (miso << 1) | evBit(i, 3);
                if (++bits == 8) {
                    pushRow(laTimes[i], 0, mosi, miso);
                    bits = 0;
                    mosi = miso = 0;
                }
            }
        }
        snprintf(modeInfo, sizeof(modeInfo), "SPI mode 0: %d bytes", rowCount);
    }

    void decode() {
        rowCount = 0;
        if (mode == M_UART) decodeUart();
        else if (mode == M_I2C) decodeI2c();
        else if (mode == M_SPI) decodeSpi();
        tableView = rowCount > 0;
        tableScroll = 0;
    }

    // ---- CSV export ----
    void saveCsv() {
        if (!rowCount) return;
        if (!SD.exists("/la")) SD.mkdir("/la");
        File f = SD.open("/la/logic.csv", FILE_WRITE);
        if (!f) return;
        f.println("time_us,kind,val,aux");
        for (int i = 0; i < rowCount; i++)
            f.printf("%lu,%u,%u,%u\n", (unsigned long)rows[i].tUs,
                     rows[i].kind, rows[i].val, rows[i].aux);
        f.close();
        snprintf(modeInfo, sizeof(modeInfo), "saved /la/logic.csv (%d rows)", rowCount);
    }

    const char* rowText(int i, char* buf, size_t sz) {
        const Row& r = rows[i];
        switch (mode) {
            case M_UART:
                if (r.kind == 3) snprintf(buf, sz, "%6lu ms  ERR 0x%02X", (unsigned long)(r.tUs / 1000), r.val);
                else snprintf(buf, sz, "%6lu ms  0x%02X '%c'", (unsigned long)(r.tUs / 1000), r.val,
                              r.val >= 0x20 && r.val < 0x7F ? r.val : '.');
                break;
            case M_I2C:
                if (r.kind == 0) snprintf(buf, sz, "%6lu ms  START", (unsigned long)(r.tUs / 1000));
                else if (r.kind == 1) snprintf(buf, sz, "%6lu ms  STOP", (unsigned long)(r.tUs / 1000));
                else if (r.kind == 2) snprintf(buf, sz, "%6lu ms  ADDR 0x%02X %s %s",
                                               (unsigned long)(r.tUs / 1000), r.val >> 1,
                                               (r.val & 1) ? "R" : "W", r.aux ? "ACK" : "NACK");
                else snprintf(buf, sz, "%6lu ms  0x%02X %s", (unsigned long)(r.tUs / 1000), r.val,
                              r.aux ? "ACK" : "NACK");
                break;
            case M_SPI:
                if (r.kind == 0) snprintf(buf, sz, "%6lu ms  M %02X <- S %02X",
                                          (unsigned long)(r.tUs / 1000), r.val, r.aux);
                else snprintf(buf, sz, "%6lu ms  segment, %u bytes", (unsigned long)(r.tUs / 1000), r.aux);
                break;
            default:
                snprintf(buf, sz, "%6lu ms  evt", (unsigned long)(r.tUs / 1000));
        }
        return buf;
    }

    // ---- v1 waveform (RAW view) ----
    void drawWaveform(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        if (laCount < 2) return;
        uint32_t maxT = laTimes[laCount - 1];
        if (maxT == 0) return;
        float scale = (float)(w - 20) / maxT;
        int highY = y0 + h / 4, lowY = y0 + h * 3 / 4;
        for (int i = 1; i < laCount && i < 400; i++) {
            uint32_t t0 = laTimes[i - 1], t1 = laTimes[i];
            int xp = x0 + 10 + (int)(t0 * scale);
            int xn = x0 + 10 + (int)(t1 * scale);
            if (xn > x0 + w - 10) xn = x0 + w - 10;
            if (xn <= xp) xn = xp + 1;
            int y = evBit(i - 1, 0) ? highY : lowY;
            c.drawLine(xp, y, xn, y, Theme::ACCENT);
            c.drawLine(xn, highY, xn, lowY, Theme::ACCENT);
        }
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "LOGIC ANALYZER 2.0");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);
        char line[56];

        if (capturing) {
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString("CAPTURING...", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            snprintf(line, sizeof(line), "events: %d / %d", laCount, LA_MAX);
            c.drawString(line, x0 + 10, y0 + 42);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: stop", x0 + 10, y0 + h - 14);
            return;
        }

        if (laCount > 0 && tableView && rowCount > 0) {
            // decoded table
            c.setTextColor(Theme::ACCENT, Theme::BG);
            c.drawString(modeInfo, x0 + 10, y0 + 20);
            int show = (h - 56) / Theme::ROW_LIST;
            if (show > 6) show = 6;
            for (int k = 0; k < show; k++) {
                int i = tableScroll + k;
                if (i >= rowCount) break;
                c.setTextColor(Theme::TEXT, Theme::BG);
                c.drawString(rowText(i, line, sizeof(line)), x0 + 10, y0 + 38 + k * Theme::ROW_LIST);
            }
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            snprintf(line, sizeof(line), "rows %d  scroll %d", rowCount, tableScroll);
            c.drawString(line, x0 + 10, y0 + h - 26);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("rot: scroll  click: back  hold: CSV", x0 + 10, y0 + h - 14);
            return;
        }

        if (laCount > 0 && !tableView) {
            // RAW waveform view
            int waveY = y0 + 58;
            int waveH = h - 92;
            c.drawRect(x0 + 8, waveY, w - 16, waveH, Theme::TEXT_DIM);
            if (mode == M_RAW) drawWaveform(c, x0 + 8, waveY, w - 16, waveH);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            snprintf(line, sizeof(line), "events: %d  span: %lu us", laCount,
                     (unsigned long)(laCount > 1 ? laTimes[laCount - 1] : 0));
            c.drawString(line, x0 + 10, y0 + 42);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString(mode == M_RAW ? "click: recapture" : "click: decode view", x0 + 10, y0 + h - 26);
            c.drawString("hold: CSV", x0 + 10, y0 + h - 14);
            return;
        }

        // idle: settings rows
        const char* rowLabels[3] = {"mode", "pin", "start"};
        for (int r = 0; r < 3; r++) {
            int y = y0 + 24 + r * 18;
            bool sel = (uiRow == r);
            c.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(rowLabels[r], x0 + 10, y);
            c.setTextColor(Theme::TEXT, Theme::BG);
            if (r == 0) {
                snprintf(line, sizeof(line), "%s%s", MODE_NAMES[mode], sel ? " <" : "");
                c.drawString(line, x0 + 70, y);
            } else if (r == 1) {
                if (mode == M_I2C) c.drawString("SDA 17  SCL 43 (fixed)", x0 + 70, y);
                else if (mode == M_SPI) c.drawString("SCK 43 CS 44 MOSI 17 MISO 1", x0 + 70, y);
                else snprintf(line, sizeof(line), "GPIO %d%s", UART_PINS[uartPinIdx], sel ? " <" : ""),
                     c.drawString(line, x0 + 70, y);
            } else {
                c.setTextColor(sel ? Theme::ACCENT : Theme::TEXT_FAINT, Theme::BG);
                c.drawString("click to capture", x0 + 70, y);
            }
        }
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("3.3V logic only (divider for 5V)", x0 + 10, y0 + h - 26);
        c.drawString("rot: select  click: edit/start", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (in.event == InputEvent::LongPress) {
            if (!capturing && rowCount > 0) { saveCsv(); Features::requestRedraw(); }
            return true;
        }
        if (in.rotationSteps != 0) {
            if (capturing) return true;
            if (laCount > 0 && tableView && rowCount > 0) {
                tableScroll = constrain(tableScroll + in.rotationSteps, 0, rowCount - 1);
                Features::requestRedraw();
                return true;
            }
            if (laCount > 0 && !tableView && mode != M_RAW) { tableView = true; Features::requestRedraw(); return true; }
            // idle: navigate rows / edit values
            if (uiRow == 0) mode = (mode + in.rotationSteps + 4) % 4;
            else if (uiRow == 1 && (mode == M_RAW || mode == M_UART))
                uartPinIdx = (uartPinIdx + in.rotationSteps + UART_PIN_N) % UART_PIN_N;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            if (capturing) { endCapture(); decode(); Features::requestRedraw(); return true; }
            if (laCount > 0) { tableView = !tableView; Features::requestRedraw(); return true; }
            if (uiRow == 2) { beginCapture(); Features::requestRedraw(); return true; }
            uiRow = (uiRow + 1) % 3;
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {
        if (capturing && laCount >= LA_MAX) {
            endCapture();
            decode();
            Features::requestRedraw();
        }
    }

    void onExit() {
        if (capturing) endCapture();
    }
}

REGISTER_FEATURE("Logic Analyzer", "Tools", MwIcon::Screwdriver, 30,
                 nullptr, input, tick, draw, onExit);
