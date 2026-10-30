// =====================================================================
//  Wi-Fi: Captive Portal — Captive Portal с собственной страницей
//  владельца. Выбор страницы: вращением из /portal/*.html (SD, иначе
//  Flash; заливаются через WebUI) либо "(built-in)" заглушка.
//  Клик — старт/стоп. Поднимает AP, DNS-wildcard + HTTP через ResMgr —
//  WebUI уходит с портов и возвращается после выключения.
//  Клиент подключается к MEGAWARE AP — телефон сам открывает страницу.
//
//  Это НЕ фишинговый шаблон: страница — то, что написал владелец.
//  Работает в фоне: выход из фичи портал не выключает (клик — стоп).
// =====================================================================
#include "../ui/features.h"
#include "../net/portal.h"
#include "../net/wifi_manager.h"
#include "../storage/files.h"
#include "../../include/theme.h"

namespace {
    int selIndex = 0; // 0 = built-in, 1..N = файлы из /portal

    FsRoot portalRoot() { return Files::sdPresent() ? FsRoot::SD : FsRoot::Flash; }

    int pageCount() { return Files::listCount(portalRoot(), "/portal") + 1; }

    String pagePathAt(int idx) {
        if (idx <= 0) return ""; // built-in
        FsEntry e = Files::listEntry(portalRoot(), "/portal", idx - 1);
        String nm(e.name);
        nm.toLowerCase();
        if (!nm.endsWith(".html") && !nm.endsWith(".htm")) return "";
        String p = String("/portal/") + e.name;
        return p;
    }

    String pageLabelAt(int idx) {
        if (idx <= 0) return "(built-in)";
        FsEntry e = Files::listEntry(portalRoot(), "/portal", idx - 1);
        return String(e.name);
    }

    void draw(LGFX_Sprite& c, int x0, int y0, int w, int h) {
        MwDraw::card(c, x0, y0, w, h, "CAPTIVE PORTAL");
        c.setFont(&fonts::Font0);
        c.setTextSize(Theme::SIZE_TEXT);
        c.setTextDatum(textdatum_t::top_left);

        if (Portal::isRunning()) {
            c.setTextColor(Theme::OK, Theme::BG);
            c.drawString("RUNNING", x0 + 10, y0 + 24);
            c.setTextColor(Theme::TEXT_DIM, Theme::BG);
            char apLine[48];
            snprintf(apLine, sizeof(apLine), "AP: MEGAWARE  %s", WifiMgr::apIP().c_str()); // no String per frame
            c.drawString(apLine, x0 + 10, y0 + 42);
            char line[48];
            snprintf(line, sizeof(line), "page: %s", Portal::pageName());
            c.drawString(line, x0 + 10, y0 + 58);
            snprintf(line, sizeof(line), "hits: %lu", (unsigned long)Portal::hits());
            c.drawString(line, x0 + 10, y0 + 74);
            c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
            c.drawString("click: stop portal", x0 + 10, y0 + h - 14);
            return;
        }

        // выбор страницы: 0 = built-in, далее /portal/*.html
        int n = pageCount();
        int visible = (h - 40) / Theme::ROW_LIST;
        int selC = (selIndex % n + n) % n;
        int top = selC - visible + 1;
        if (top < 0) top = 0;
        c.setTextDatum(textdatum_t::middle_left);
        for (int i = 0; i < visible && top + i < n; i++) {
            int idx = top + i;
            bool s = (idx == selC);
            int y = y0 + 20 + i * Theme::ROW_LIST + Theme::ROW_LIST / 2;
            if (s) { c.setTextColor(Theme::ACCENT, Theme::BG); c.drawString(">", x0, y); }
            c.setTextColor(s ? Theme::TEXT : Theme::TEXT_DIM, Theme::BG);
            String lbl = pageLabelAt(idx);
            if (idx > 0) { String lower = lbl; lower.toLowerCase();
                if (!lower.endsWith(".html") && !lower.endsWith(".htm")) lbl += " (skip)"; }
            c.drawString(lbl, x0 + 12, y);
        }
        c.setTextDatum(textdatum_t::top_left);
        c.setTextColor(Theme::TEXT_FAINT, Theme::BG);
        c.drawString("rotate: page  click: start portal", x0 + 10, y0 + h - 14);
    }

    bool input(const InputState& in) {
        if (Portal::isRunning()) {
            if (in.event == InputEvent::Click) {
                Portal::stop();
                Features::requestRedraw();
                return true;
            }
            return false;
        }
        int n = pageCount();
        if (in.rotationSteps != 0) {
            selIndex = ((selIndex + in.rotationSteps) % n + n) % n;
            Features::requestRedraw();
            return true;
        }
        if (in.event == InputEvent::Click) {
            int selC = (selIndex % n + n) % n;
            String path = pagePathAt(selC);
            if (path.isEmpty()) {
                // выбран не-.html файл или built-in — built-in стартует,
                // не-html: стартуем built-in (страницу можно выбрать позже)
                path = "";
            }
            Portal::start(path);
            Features::requestRedraw();
            return true;
        }
        return false;
    }

    void tick() {}

    void onExit() {
        // выход из фичи НЕ выключает портал — он работает в фоне
        // (Portal::loop() зовётся из главного тика). Стоп — клик.
    }
}

REGISTER_FEATURE("Captive Portal", MwCat::Wifi, MwIcon::Lock, 20,
                 nullptr, input, tick, draw, onExit);
