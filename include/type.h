//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_TYPE_H
#define YADDNSC_TYPE_H

enum class ip_version_t {
    UNSPECIFIED, IPV4, IPV6
};

enum class dns_record_t {
    A, AAAA, TXT
};

#endif //YADDNSC_TYPE_H
