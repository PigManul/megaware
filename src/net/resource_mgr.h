#pragma once
#include <Arduino.h>

// =====================================================================
//  ResMgr — арбитр сетевых ресурсов между WebUI и фичами.
//
//  WebUI — НЕ хозяин портов, а такой же потребитель, как любая фича.
//  Фича (captive portal, свой HTTP/DNS-сервер, REST) перед работой
//  захватывает ресурс, по завершении — отпускает:
//
//    ResMgr::claim(ResMgr::Resource::Http80, "my_feature");   // onEnter
//    ResMgr::release(ResMgr::Resource::Http80, "my_feature"); // onExit
//
//  WebUI следит за клеймами в WebUI::loop() и сам уходит с дороги
//  (останавливает свой WebServer/DNSServer) и сам возвращается, когда
//  ресурс освободился. Пока ресурс клеймен, WebUI::isRunning() честно
//  сообщает false — «включён, но preempted».
//
//  Не конфликтуют (арбитраж не нужен): исходящий интернет из фич
//  (HTTPClient, MQTT, WiFiClient) и сам AP-режим (AP+STA общий).
// =====================================================================

namespace ResMgr {
    enum class Resource : uint8_t {
        Http80, // TCP :80 — WebServer WebUI / свой HTTP-сервер фичи
        Dns53,  // UDP :53 — DNSServer WebUI / свой DNS фичи (captive)
    };

    // true — ресурс у тебя (или уже был у тебя). false — занят другой фичей.
    bool claim(Resource r, const char* owner);
    // Идемпотентно; с чужим owner — no-op.
    void release(Resource r, const char* owner);
    // Занят ли кем-то (WebUI по этому флагу уходит с ресурса).
    bool isHeld(Resource r);
}
