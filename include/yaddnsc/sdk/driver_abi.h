//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_DRIVER_ABI_H
#define YADDNSC_SDK_DRIVER_ABI_H

/*
 * yaddnsc v1 alpha plugin ABI — the ONLY stable surface across the .so
 * boundary.
 *
 * This header is pure C and may only depend on <stddef.h> and <stdint.h>;
 * it must never include C++ STL, third-party libraries, or host-internal
 * headers. Plugins are compiled with the same C++ standard as the host
 * (C++23) — see docs/custom-drivers.md.
 *
 * struct_size convention: every extensible struct carries its caller- or
 * provider-supplied size in bytes as its first field. A reader may only
 * read fields fully covered by struct_size (see yaddnsc_struct_has_field);
 * appending fields is allowed within one api_revision, anything else
 * requires bumping YADDNSC_DRIVER_API_REVISION.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YADDNSC_DRIVER_API_REVISION UINT32_C(1)
#define YADDNSC_DRIVER_MAGIC UINT64_C(0x594144444E534300)

/* ── View types ───────────────────────────────────────────────────────────
 * Borrowed views: `data` is not necessarily NUL-terminated, `size` is the
 * only valid length. size == 0 with data == NULL is legal; size > 0 with
 * data == NULL is invalid. Views expire when the call returns unless a
 * stricter lifetime is documented for the specific callback.
 */
typedef struct yaddnsc_string {
    const char *data;
    size_t size;
} yaddnsc_string;

typedef struct yaddnsc_bytes {
    const uint8_t *data;
    size_t size;
} yaddnsc_bytes;

/* ── Status codes ─────────────────────────────────────────────────────────*/
typedef uint32_t yaddnsc_status;
#define YADDNSC_STATUS_OK UINT32_C(0)
#define YADDNSC_STATUS_INVALID_ARGUMENT UINT32_C(1)
#define YADDNSC_STATUS_INVALID_CONFIG UINT32_C(2)
#define YADDNSC_STATUS_UNSUPPORTED_RECORD UINT32_C(3)
#define YADDNSC_STATUS_NETWORK_ERROR UINT32_C(4)
#define YADDNSC_STATUS_AUTHENTICATION_FAILED UINT32_C(5)
#define YADDNSC_STATUS_RATE_LIMITED UINT32_C(6)
#define YADDNSC_STATUS_UPSTREAM_REJECTED UINT32_C(7)
#define YADDNSC_STATUS_INVALID_RESPONSE UINT32_C(8)
#define YADDNSC_STATUS_CANCELLED UINT32_C(9)
#define YADDNSC_STATUS_INTERNAL_ERROR UINT32_C(10)

/* ── Log levels ───────────────────────────────────────────────────────────*/
typedef uint32_t yaddnsc_log_level;
#define YADDNSC_LOG_TRACE UINT32_C(0)
#define YADDNSC_LOG_DEBUG UINT32_C(1)
#define YADDNSC_LOG_INFO UINT32_C(2)
#define YADDNSC_LOG_WARN UINT32_C(3)
#define YADDNSC_LOG_ERROR UINT32_C(4)

/* Source location — core payload of `log`, mirroring __FILE__ / __LINE__ /
 * __FUNCTION__. Leaf struct (no struct_size). For `log` calls: `location`
 * must not be NULL, file/function must be non-empty views, line must be > 0.
 */
typedef struct yaddnsc_source_location {
    yaddnsc_string file;
    int32_t line;
    yaddnsc_string function;
} yaddnsc_source_location;

/* ── HTTP ─────────────────────────────────────────────────────────────────*/
typedef uint32_t yaddnsc_http_method;
#define YADDNSC_HTTP_GET UINT32_C(1)
#define YADDNSC_HTTP_POST UINT32_C(2)
#define YADDNSC_HTTP_PUT UINT32_C(3)
#define YADDNSC_HTTP_DELETE UINT32_C(4)
#define YADDNSC_HTTP_PATCH UINT32_C(5)
#define YADDNSC_HTTP_HEAD UINT32_C(6)
#define YADDNSC_HTTP_OPTIONS UINT32_C(7)

#define YADDNSC_DRIVER_CAPABILITY_A (UINT64_C(1) << 0)
#define YADDNSC_DRIVER_CAPABILITY_AAAA (UINT64_C(1) << 1)

/* Caller-owned output: the caller sets struct_size to its capacity; the
 * writer only writes fields fully inside that capacity, never writes its
 * own sizeof back beyond capacity, and never zeroes an unknown tail.
 */
typedef struct yaddnsc_error {
    uint32_t struct_size;
    yaddnsc_status status;
    uint32_t retry_after_seconds;
    yaddnsc_string message;
} yaddnsc_error;

typedef struct yaddnsc_http_header {
    yaddnsc_string name;
    yaddnsc_string value;
} yaddnsc_http_header;

typedef struct yaddnsc_http_request {
    uint32_t struct_size;
    yaddnsc_string url;
    yaddnsc_http_method method;
    const yaddnsc_http_header *headers;
    size_t header_count;
    yaddnsc_string content_type;
    yaddnsc_bytes body;
} yaddnsc_http_request;

typedef struct yaddnsc_http_response {
    uint32_t struct_size;
    uint32_t status_code;
    const yaddnsc_http_header *headers;
    size_t header_count;
    yaddnsc_bytes body;
} yaddnsc_http_response;

/* driver_param_json: host-serialised, valid JSON UTF-8 bytes; owned by the
 * host, valid for the duration of yaddnsc_driver_update() only.
 */
typedef struct yaddnsc_update_request {
    uint32_t struct_size;
    yaddnsc_string ip_address;
    yaddnsc_string record_type;
    yaddnsc_string domain;
    yaddnsc_string subdomain;
    yaddnsc_string fqdn;
    yaddnsc_bytes driver_param_json;
} yaddnsc_update_request;

/* Returned from static plugin storage; the provider sets struct_size to the
 * real size of the struct and every string view must point to static storage.
 */
typedef struct yaddnsc_driver_descriptor {
    uint32_t struct_size;
    uint32_t api_revision;
    uint64_t magic;
    yaddnsc_string name;
    yaddnsc_string version;
    yaddnsc_string author;
    yaddnsc_string description;
    uint64_t capabilities;
} yaddnsc_driver_descriptor;

typedef struct yaddnsc_host_services yaddnsc_host_services;
typedef struct yaddnsc_driver yaddnsc_driver;

/* Host Services — the only host capabilities a plugin may call. `context`
 * is host-owned and opaque: the plugin passes it back verbatim. Valid from
 * yaddnsc_driver_create() until the matching yaddnsc_driver_destroy().
 *
 * log:           returns void — logging failure must never fail an update.
 * http_exchange: non-OK means transport/cancellation failure only; provider
 *                HTTP status always arrives via out_response->status_code.
 *                Response views stay valid until yaddnsc_driver_update()
 *                returns (host arena), across multiple exchanges.
 * is_cancelled:  0 = not cancelled, non-0 = cancelled (host-global shutdown
 *                signal). A predicate, not a fallible operation.
 */
struct yaddnsc_host_services {
    uint32_t struct_size;
    uint32_t api_revision;
    void *context;
    void (*log)(void *context, yaddnsc_log_level level,
                yaddnsc_string message,
                const yaddnsc_source_location *location);
    yaddnsc_status (*http_exchange)(void *context,
                                    const yaddnsc_http_request *request,
                                    yaddnsc_http_response *out_response,
                                    yaddnsc_error *out_error);
    int (*is_cancelled)(void *context);
};

/* ── Exported entry points (extern "C", no exceptions, no C++ objects) ────*/

yaddnsc_status yaddnsc_driver_get_descriptor(
    const yaddnsc_driver_descriptor **out_descriptor);

yaddnsc_status yaddnsc_driver_create(
    const yaddnsc_host_services *services,
    yaddnsc_driver **out_driver,
    yaddnsc_error *out_error);

void yaddnsc_driver_destroy(yaddnsc_driver *driver);

yaddnsc_status yaddnsc_driver_update(
    yaddnsc_driver *driver,
    const yaddnsc_update_request *request,
    yaddnsc_error *out_error);

/* ── struct_size helpers (C and C++) ──────────────────────────────────────*/

/* Byte size covering `member` of `type` completely — the only legal test
 * for field visibility. All arithmetic is compile-time constant. */
#define YADDNSC_SIZEOF_THROUGH(type, member) \
    ((uint32_t)(offsetof(type, member) + sizeof(((type *)0)->member)))

/* True when `field_end` (YADDNSC_SIZEOF_THROUGH of the field) is fully
 * covered by the supplied struct_size. */
static inline int yaddnsc_struct_has_field(uint32_t struct_size, uint32_t field_end) {
    return struct_size >= field_end;
}

/* Minimum accepted struct_size (required prefix) per struct. */
#define YADDNSC_ERROR_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_error, message)
#define YADDNSC_HTTP_REQUEST_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_http_request, body)
#define YADDNSC_HTTP_RESPONSE_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_http_response, body)
#define YADDNSC_UPDATE_REQUEST_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_update_request, driver_param_json)
#define YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_driver_descriptor, capabilities)
#define YADDNSC_HOST_SERVICES_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_host_services, is_cancelled)

/* ── Layout assertions ────────────────────────────────────────────────────
 * Pin every field offset so an accidental reorder/insert trips the build
 * instead of silently breaking the ABI (LP64/LLP64 layout).
 */
#define YADDNSC_ABI_ASSERT_OFFSET(type, member, expected) \
    YADDNSC_STATIC_ASSERT(offsetof(type, member) == (expected), #type "." #member " offset changed — bump api_revision")

#if defined(__cplusplus)
#define YADDNSC_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define YADDNSC_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

YADDNSC_STATIC_ASSERT(sizeof(void *) == 8, "the v1 alpha ABI assumes a 64-bit platform");
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_string) == 16, "yaddnsc_string layout changed");
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_bytes) == 16, "yaddnsc_bytes layout changed");

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_source_location, file, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_source_location, line, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_source_location, function, 24);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_error, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_error, status, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_error, retry_after_seconds, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_error, message, 16);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_header, name, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_header, value, 16);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, url, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, method, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, headers, 32);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, header_count, 40);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, content_type, 48);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_request, body, 64);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_response, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_response, status_code, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_response, headers, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_response, header_count, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_http_response, body, 24);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, ip_address, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, record_type, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, domain, 40);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, subdomain, 56);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, fqdn, 72);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_update_request, driver_param_json, 88);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, api_revision, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, magic, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, name, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, version, 32);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, author, 48);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, description, 64);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, capabilities, 80);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, api_revision, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, context, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, log, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, http_exchange, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, is_cancelled, 32);

#undef YADDNSC_ABI_ASSERT_OFFSET
#undef YADDNSC_STATIC_ASSERT

#ifdef __cplusplus
}
#endif

#endif /* YADDNSC_SDK_DRIVER_ABI_H */
