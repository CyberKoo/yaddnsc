//
// Created by Kotarou on 2026/7/3.
//

#ifndef YADDNSC_DNS_TYPE_H
#define YADDNSC_DNS_TYPE_H

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// DNS — shared types used across config and DNS layers.
// ---------------------------------------------------------------------------
namespace DNS {
    enum class Type {
        A, AAAA, TXT, SOA
    };

    struct Server {
        std::string address;
        uint16_t port{53};
    };
}

#endif // YADDNSC_DNS_TYPE_H
