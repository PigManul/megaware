#include "dissect.h"
#include <string.h>

namespace {
    // Build the level-cell stream: consecutive runs of constant level,
    // measured in timing units. First level assumed HIGH (GDO2 idle low,
    // first edge raises it) - the relative pattern is what matters.
    int buildCells(const RawCapture& cap, uint32_t unitUs, uint8_t* cells, int maxCells) {
        int n = 0;
        bool level = true;
        for (size_t i = 0; i < cap.count && n < maxCells; i++) {
            uint32_t u = (cap.durationsUs[i] + unitUs / 2) / unitUs;
            if (u < 1) u = 1;
            for (uint32_t k = 0; k < u && n < maxCells; k++) cells[n++] = level ? 1 : 0;
            level = !level;
        }
        return n;
    }

    // collapse cells back into run lengths (level alternates each run)
    int buildRuns(const uint8_t* cells, int cellsN, uint16_t* runLen, int maxRuns) {
        int runs = 0;
        int i = 0;
        while (i < cellsN && runs < maxRuns) {
            int j = i;
            while (j < cellsN && cells[j] == cells[i]) j++;
            runLen[runs++] = (uint16_t)(j - i);
            i = j;
        }
        return runs;
    }

    void pushBit(DissectResult& r, uint8_t b) {
        if (r.bitCount >= (int)sizeof(r.bits) * 8) return;
        if (r.bitCount % 8 == 0) r.bits[r.bitCount / 8] = 0;
        if (b) r.bits[r.bitCount / 8] |= (0x80 >> (r.bitCount % 8));
        r.bitCount++;
    }

    void pushDigit(DissectResult& r, uint8_t d) {
        if (r.digitCount >= (int)sizeof(r.digits)) return;
        r.digits[r.digitCount++] = d;
    }

    // longest alternating 0101... prefix
    int preambleLen(const uint8_t* bits, int n) {
        int i = 0;
        while (i + 1 < n && bits[i] != bits[i + 1]) i++;
        return i + 1;
    }

    // PWM/PPM: one digit per (hi, lo) pulse pair.
    //   short-hi + long-lo  -> 0
    //   long-hi + short-lo  -> 1
    //   equal halves        -> 2 (tri-state F, PT-2262 open pin)
    // Returns digit count, or 0 if the framing is too broken.
    int decodePpm(const uint16_t* runLen, int runs, DissectResult& r) {
        if (runs < 4) return 0;
        int clean = 0, total = 0;
        int i = 0;
        while (i + 1 < runs) {
            uint32_t hi = runLen[i], lo = runLen[i + 1];
            total++;
            if (hi * 100 < lo * 55) { pushDigit(r, 0); clean++; }
            else if (lo * 100 < hi * 55) { pushDigit(r, 1); clean++; }
            else if (hi * 130 > lo * 100 && lo * 130 > hi * 100) { pushDigit(r, 2); clean++; }
            else break; // framing broken (noise tail)
            i += 2;
        }
        if (total < 4 || clean * 100 < total * 85) return 0;
        return r.digitCount;
    }
}

bool Dissect::analyze(const RawCapture& cap, DissectResult& out) {
    out = DissectResult();
    if (!cap.valid || cap.count < 8) return false;

    // timing unit = shortest pulse
    uint32_t minUs = 0xFFFFFFFF;
    for (size_t i = 0; i < cap.count; i++)
        if (cap.durationsUs[i] < minUs && cap.durationsUs[i] >= 40) minUs = cap.durationsUs[i];
    if (minUs == 0xFFFFFFFF) return false;
    out.unitUs = minUs;
    out.baud = 1000000UL / minUs;

    static uint8_t cells[2048];
    int cellsN = buildCells(cap, out.unitUs, cells, 2048);
    if (cellsN < 8) return false;

    static uint16_t runs[1024];
    int runsN = buildRuns(cells, cellsN, runs, 1024);

    // --- PPM/PWM candidate ---
    DissectResult ppm = out;
    int ppmDigits = decodePpm(runs, runsN, ppm);

    // measured long/short ratio + EV1527-style sync gap
    float ratio = 0;
    bool longGap = false;
    {
        float sSum = 0, lSum = 0;
        int pairs = 0;
        for (int i = 0; i + 1 < runsN && pairs < 64; i += 2) {
            uint32_t a = runs[i], b = runs[i + 1];
            uint32_t s = a < b ? a : b, l = a < b ? b : a;
            if (s >= 1 && l >= 2 * s) { sSum += s; lSum += l; pairs++; }
        }
        if (pairs >= 8) ratio = lSum / sSum;
        for (int i = 0; i < runsN; i++) if (runs[i] >= 8) { longGap = true; break; }
    }

    // --- Manchester candidate: pairs of cells, both polarities ---
    int manScore[2] = {0, 0};
    static DissectResult man[2];
    int manBest = -1;
    if (cellsN >= 16 && cellsN % 2 == 0) {
        for (int inv = 0; inv < 2; inv++) {
            man[inv] = out;
            for (int i = 0; i + 1 < cellsN; i += 2)
                pushBit(man[inv], cells[i + 1] ^ inv); // 01 -> 1 (rising mid-bit)
            manScore[inv] = preambleLen(man[inv].bits, man[inv].bitCount > 64 ? 64 : man[inv].bitCount);
        }
        manBest = manScore[1] > manScore[0] ? 1 : 0;
    }

    // --- NRZ: long constant-level runs present ---
    bool hasLongRuns = false;
    for (int i = 0; i < runsN; i++) if (runs[i] >= 3) { hasLongRuns = true; break; }

    // pick the encoding: PPM wins when it produced a clean digit stream
    if (ppmDigits >= 8) {
        out.enc = 0;
        out.encName = "PWM/PPM";
        out.digitCount = ppm.digitCount;
        memcpy(out.digits, ppm.digits, sizeof(out.digits));
        // bit view: 2 bits per digit (hiLong, loLong): 0->01, 1->10, F->00
        for (int d = 0; d < ppm.digitCount; d++) {
            pushBit(out, ppm.digits[d] == 1);
            pushBit(out, ppm.digits[d] == 0);
        }
    } else if (manBest >= 0 && manScore[manBest] >= 6) {
        out.enc = 1;
        out.encName = "Manchester";
        memcpy(out.bits, man[manBest].bits, sizeof(out.bits));
        out.bitCount = man[manBest].bitCount;
    } else if (hasLongRuns) {
        out.enc = 2;
        out.encName = "NRZ";
        for (int i = 0; i < cellsN; i++) pushBit(out, cells[i]);
    } else {
        out.enc = 3;
        out.encName = "unknown";
        for (int i = 0; i < cellsN; i++) pushBit(out, cells[i]);
    }
    if (out.bitCount < 8 && out.digitCount < 4) return false;

    // preamble + sync over the bit view
    if (out.bitCount >= 16) {
        out.preambleBits = preambleLen(out.bits, out.bitCount > 96 ? 96 : out.bitCount);
        if (out.preambleBits >= 8 && out.preambleBits + 16 <= out.bitCount) {
            int p = out.preambleBits;
            uint16_t s = 0;
            for (int i = 0; i < 16; i++)
                if (out.bits[(p + i) / 8] & (0x80 >> ((p + i) % 8))) s |= (0x8000 >> i);
            out.sync = s;
        }
    }

    // --- protocol database ---
    char hex[130];
    if (out.enc == 0 && out.digitCount == 12) {
        bool hasF = false;
        for (int d = 0; d < 12; d++) if (out.digits[d] == 2) hasF = true;
        if (hasF) {
            out.proto = "Princeton PT-2262";
            char dig[13];
            for (int d = 0; d < 12; d++) dig[d] = out.digits[d] == 2 ? 'F' : ('0' + out.digits[d]);
            dig[12] = 0;
            uint32_t btn = 0;
            for (int d = 8; d < 12; d++) btn = (btn << 1) | (out.digits[d] & 1);
            snprintf(out.summary, sizeof(out.summary), "addr %.8s, buttons %.4s (0x%02lX)",
                     dig, dig + 8, (unsigned long)btn);
        } else if (ratio > 0 && ratio < 2.55f) {
            out.proto = "CAME / Nice Flo";
            uint32_t v = 0;
            for (int d = 0; d < 12; d++) v = (v << 1) | (out.digits[d] & 1);
            snprintf(out.summary, sizeof(out.summary), "12-bit gate, code 0x%03X (k=%.1f)",
                     (unsigned)v, (double)ratio);
        } else {
            out.proto = "Holtek-like 12-bit";
            uint32_t v = 0;
            for (int d = 0; d < 12; d++) v = (v << 1) | (out.digits[d] & 1);
            // HT-12E family: 8 address + 4 data bits
            snprintf(out.summary, sizeof(out.summary), "addr 0x%02X data 0x%X (k=%.1f)",
                     (unsigned)(v >> 4), (unsigned)(v & 0xF), (double)ratio);
        }
    } else if (out.enc == 0 && out.digitCount == 24) {
        bool hasF = false;
        for (int d = 0; d < 24; d++) if (out.digits[d] == 2) hasF = true;
        uint32_t v = 0;
        for (int d = 0; d < 24; d++) v = (v << 1) | (out.digits[d] & 1);
        if (longGap || ratio >= 2.55f) {
            out.proto = "EV1527";
            // EV1527: 24-bit PWM 1:3, frames separated by a long sync gap;
            // typical remotes: 20 addr + 4 button bits
            snprintf(out.summary, sizeof(out.summary), "code 0x%06lX (addr %05lX btn %lX, k=%.1f)",
                     (unsigned long)v, (unsigned long)(v >> 4), (unsigned long)(v & 0xF),
                     (double)(ratio ? ratio : 3.0f));
        } else if (hasF) {
            out.proto = "PT-2262 ext";
            char dig[26];
            for (int d = 0; d < 24; d++) dig[d] = out.digits[d] == 2 ? 'F' : ('0' + out.digits[d]);
            dig[24] = 0;
            snprintf(out.summary, sizeof(out.summary), "24 digits: %.24s", dig);
        } else {
            out.proto = "24-bit fixed code";
            snprintf(out.summary, sizeof(out.summary), "code 0x%06lX (addr %05lX btn %lX)",
                     (unsigned long)v, (unsigned long)(v >> 4), (unsigned long)(v & 0xF));
        }
    } else if (out.enc == 0 && out.digitCount >= 16 && out.digitCount <= 28) {
        out.proto = "PPM remote";
        char dig[30];
        int show = out.digitCount < 28 ? out.digitCount : 28;
        for (int d = 0; d < show; d++) dig[d] = out.digits[d] == 2 ? 'F' : ('0' + out.digits[d]);
        dig[show] = 0;
        snprintf(out.summary, sizeof(out.summary), "%d digits: %s (k=%.1f)", out.digitCount, dig,
                 (double)ratio);
    } else if (out.enc == 1) {
        out.proto = "Manchester frame";
        Dissect::hexOf(out, hex, sizeof(hex));
        if (out.sync)
            snprintf(out.summary, sizeof(out.summary), "pre %db sync 0x%04X: %s",
                     out.preambleBits, out.sync, hex);
        else
            snprintf(out.summary, sizeof(out.summary), "%d bits: %s", out.bitCount, hex);
    } else if (out.enc == 2) {
        out.proto = "NRZ frame";
        Dissect::hexOf(out, hex, sizeof(hex));
        snprintf(out.summary, sizeof(out.summary), "~%lu us/cell, %d bits: %s",
                 (unsigned long)out.unitUs, out.bitCount, hex);
    } else {
        Dissect::hexOf(out, hex, sizeof(hex));
        snprintf(out.summary, sizeof(out.summary), "%d cells: %s", out.bitCount, hex);
    }

    out.valid = true;
    out.ratio = ratio;
    out.longGap = longGap;
    return true;
}

void Dissect::hexOf(const DissectResult& r, char* buf, size_t bufsz) {
    size_t o = 0;
    int bytes = (r.bitCount + 7) / 8;
    if (bytes > 64) bytes = 64;
    for (int i = 0; i < bytes && o + 2 < bufsz; i++) {
        snprintf(buf + o, bufsz - o, "%02X", r.bits[i]);
        o += 2;
    }
    if (o < bufsz) buf[o] = 0;
}

void Dissect::dewhitenPn9(DissectResult& r) {
    // CC1101 whitening: PN9 keystream, 9-bit LFSR, seed 0x1FF,
    // poly x^9+x^5+1, output LSB first, one fresh 9-bit phase per byte
    uint16_t lfsr = 0x1FF;
    int bytes = (r.bitCount + 7) / 8;
    for (int i = 0; i < bytes; i++) {
        uint8_t ks = 0;
        for (int b = 0; b < 8; b++) {  // keystream bits LSB-first
            ks >>= 1;
            if (lfsr & 1) ks |= 0x80;
            uint8_t fb = ((lfsr & 1) ^ ((lfsr >> 5) & 1)) & 1;
            lfsr = (lfsr >> 1) | (fb << 8);
        }
        r.bits[i] ^= ks;
    }
    r.dewhitened = true;
}
