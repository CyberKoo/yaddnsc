//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_URL_ENCODE_HPP
#define YADDNSC_SDK_URL_ENCODE_HPP

/// Percent-encoding for driver plugins (RFC 3986 §2.1), used by cloud API
/// signing schemes (Alibaba Cloud RPC, AWS SigV4 canonical URIs). The
/// implementation lives in <yaddnsc/util/url_encode.hpp>; this header exposes
/// it under the SDK namespace.

#include <yaddnsc/util/url_encode.hpp>

namespace yaddnsc::sdk {

using util::url_encode;

} // namespace yaddnsc::sdk

#endif // YADDNSC_SDK_URL_ENCODE_HPP
