//
// Created by Kotarou on 2026/7/7.
//

#ifndef YADDNSC_DNS_PARSER_H
#define YADDNSC_DNS_PARSER_H

// Unified entry point for DNS response parsing.
//
// The actual parser implementation is selected at compile time:
//   - YADDNSC_USE_NATIVE_DNS=1 → self-contained parser (EXPERIMENTAL)
//   - YADDNSC_USE_NATIVE_DNS=0 → system libresolv-based parser (STABLE, default)
//
// Both expose the same core API under DNS::RecordParser.

#if YADDNSC_USE_NATIVE_DNS
#  include "dns/parser/parser_native.h"
#else
#  include "dns/parser/parser_system.h"
#endif

#endif  // YADDNSC_DNS_PARSER_H
