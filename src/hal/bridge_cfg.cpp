#include "bridge_cfg.h"
#include "config.h"

namespace {
    uint32_t ver = 0;
}

void BridgeCfg::load(BridgeRule out[SLOTS]) {
    for (int i = 0; i < SLOTS; i++) {
        char k[20];
        snprintf(k, sizeof(k), "br%d_en", i);
        out[i].enabled = Config::getBool(k, false);
        snprintf(k, sizeof(k), "br%d_lp", i);
        out[i].lport = (uint16_t)Config::getInt(k, 0);
        snprintf(k, sizeof(k), "br%d_rp", i);
        out[i].rport = (uint16_t)Config::getInt(k, 0);
        snprintf(k, sizeof(k), "br%d_host", i);
        String h = Config::getString(k, "");
        strncpy(out[i].host, h.c_str(), sizeof(out[i].host) - 1);
        out[i].host[sizeof(out[i].host) - 1] = 0;
    }
    ver = (uint32_t)Config::getInt("br_ver", 0);
}

void BridgeCfg::save(int slot, const BridgeRule& r) {
    if (slot < 0 || slot >= SLOTS) return;
    char k[20];
    snprintf(k, sizeof(k), "br%d_en", slot);
    Config::setBool(k, r.enabled);
    snprintf(k, sizeof(k), "br%d_lp", slot);
    Config::setInt(k, r.lport);
    snprintf(k, sizeof(k), "br%d_rp", slot);
    Config::setInt(k, r.rport);
    snprintf(k, sizeof(k), "br%d_host", slot);
    Config::setString(k, r.host);
    ver++;
    Config::setInt("br_ver", (int)ver);
}

uint32_t BridgeCfg::version() { return ver; }
