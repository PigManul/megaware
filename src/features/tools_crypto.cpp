// =====================================================================
//  Tools: Crypto — AES-256-ECB encrypt/decrypt + SHA-256 hash
//  on the ESP32-S3 hardware accelerator (mbedTLS).
//  Input: file /crypto/input.txt via the WebUI. Key: /crypto/key.txt.
//  Click — encrypt/decrypt/hash, result on screen + /crypto/output.txt.
// =====================================================================
#include "../ui/features.h"
#include "../storage/files.h"
#include "../../include/theme.h"
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <SD.h>
#include <LittleFS.h>

namespace {
    enum Action : uint8_t { Enc, Dec, Hash };
    uint8_t action = Enc;
    char statusMsg[64] = "";
    uint32_t statusUntil = 0;
    bool statusOk = false;

    fs::FS& cryptoFs() { return Files::sdPresent() ? (fs::FS&)SD : (fs::FS&)LittleFS; }

    void readFile(const char* path, String& out) {
        File f = cryptoFs().open(path, FILE_READ);
        if (!f) return;
        out = f.readString();
        f.close();
        out.trim();
    }

    void setStatus(const char* m, bool ok) {
        snprintf(statusMsg, sizeof(statusMsg), "%s", m);
        statusUntil = millis() + 4000;
        statusOk = ok;
    }

    void doAction() {
        String input, keyStr;
        readFile("/crypto/input.txt", input);
        readFile("/crypto/key.txt", keyStr);
        if (!input.length()) { setStatus("no /crypto/input.txt", false); return; }

        if (action == Hash) {
            uint8_t sha[32];
            mbedtls_sha256((const uint8_t*)input.c_str(), input.length(), sha, 0);
            String hex;
            for (int i = 0; i < 32; i++) hex += String(sha[i] < 16 ? "0" : "") + String(sha[i], 16);
            File o = cryptoFs().open("/crypto/output.txt", FILE_WRITE);
            if (o) { o.print(hex); o.close(); }
            snprintf(statusMsg, sizeof(statusMsg), "SHA256: %s", hex.substring(0, 16).c_str());
            statusUntil = millis() + 5000;
            statusOk = true;
            return;
        }

        // AES-256-ECB: key = SHA256(keyStr) (32 bytes)
        uint8_t key[32];
        mbedtls_sha256((const uint8_t*)keyStr.c_str(), keyStr.length(), key, 0);
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, key, 256);

        // PKCS7 pad
        size_t inLen = input.length();
        size_t padLen = 16 - (inLen % 16);
        size_t padded = inLen + padLen;
        uint8_t* buf = (uint8_t*)malloc(padded);
        if (!buf) { setStatus("out of memory", false); return; }
        memcpy(buf, input.c_str(), inLen);
        memset(buf + inLen, padLen, padLen);

        if (action == Enc) {
            for (size_t off = 0; off < padded; off += 16)
                mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, buf + off, buf + off);
            // hex output
            String hex;
            for (size_t i = 0; i < padded; i++) hex += String(buf[i] < 16 ? "0" : "") + String(buf[i], 16);
            File o = cryptoFs().open("/crypto/output.txt", FILE_WRITE);
            if (o) { o.print(hex); o.close(); }
            snprintf(statusMsg, sizeof(statusMsg), "encrypted %u B (hex)", (unsigned)padded);
            statusUntil = millis() + 4000;
            statusOk = true;
        } else {
            // decrypt: input is hex
            size_t hexLen = input.length();
            padded = hexLen / 2;
            for (size_t i = 0; i + 1 < hexLen; i += 2) {
                char two[3] = {input[i], input[i+1], 0};
                buf[i/2] = (uint8_t)strtol(two, nullptr, 16);
            }
            padded = hexLen / 2;
            for (size_t off = 0; off < padded; off += 16)
                mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, buf + off, buf + off);
            // strip PKCS7
            if (padded > 0) {
                uint8_t pad = buf[padded - 1];
                if (pad <= 16 && pad > 0) padded -= pad;
            }
            buf[padded] = 0;
            File o = cryptoFs().open("/crypto/output.txt", FILE_WRITE);
            if (o) { o.write(buf, padded); o.close(); }
            snprintf(statusMsg, sizeof(statusMsg), "decrypted %u B", (unsigned)padded);
            statusUntil = millis() + 4000;
            statusOk = true;
        }
        free(buf);
        mbedtls_aes_free(&aes);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "CRYPTO");
        c.setFont(&fonts::Font0);
        c.setTextDatum(textdatum_t::top_left);
        const char* items[3] = {"AES-256 Encrypt", "AES-256 Decrypt", "SHA-256 Hash"};
        c.setTextDatum(textdatum_t::middle_left);
        for (int i = 0; i < 3; i++) {
            int y = y0 + 24 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            bool sel = (action == i);
            if (sel) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(sel ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            c.drawString(items[i], x0 + 12, y);
        }
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("in: /crypto/input.txt  key: /crypto/key.txt", x0 + 10, y0 + h - 42);
        c.drawString("out: /crypto/output.txt", x0 + 10, y0 + h - 28);
        c.drawString("upload via WebUI, click = apply", x0 + 10, y0 + h - 14);
        if (statusMsg[0] && millis() < statusUntil) {
            c.setTextColor(statusOk ? Theme::OK : Theme::ERR, Theme::BG);
            c.drawString(statusMsg, x0 + 10, y0 + h - 56);
        }
    }

    bool input(const InputState& in) {
        if (in.rotationSteps != 0) {
            action = (action + in.rotationSteps) % 3;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            doAction();
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}
}

REGISTER_FEATURE("Crypto", "Tools", MwIcon::Screwdriver, 30,
                 nullptr, input, tick, draw, nullptr);
