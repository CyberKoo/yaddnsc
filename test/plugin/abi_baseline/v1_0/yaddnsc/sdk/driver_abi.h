/*
 * DO NOT EDIT — frozen ABI 1.0 baseline.
 *
 * Byte-for-byte snapshot of include/yaddnsc/sdk/driver_abi.h taken at the
 * ABI 1.0 freeze (plus this banner). Guarded by driver_abi.h.sha256, checked
 * in CI. The frozen v1.0 plugin (test/plugin/abi_baseline/frozen_v1_0_plugin.cpp)
 * compiles against THIS copy, so a host-side change that breaks ABI 1.0
 * compatibility turns test_abi_freeze_compat red.
 *
 * Never modify this file. ABI evolution appends to the LIVE header only;
 * see docs/development.md#plugin-abi-changes.
 */
#ifndef YADDNSC_SDK_DRIVER_ABI_H
#define YADDNSC_SDK_DRIVER_ABI_H

/*
 * yaddnsc v1 plugin ABI — the ONLY stable surface across the .so
 * boundary.
 *
 * Stable since ABI 1.0: a plugin built against ABI 1.0 loads and runs on
 * every host implementing ABI 1.x. The versioning rules below are the
 * contract that makes that promise hold. One plugin binary serves exactly
 * one ABI major: the exported entry-point symbol names are fixed, so a
 * single .so cannot offer v1 and a future v2 side by side.
 *
 * Supported ABI:
 *   - Linux ELF x86_64, Linux ELF AArch64, macOS 64-bit
 *   - C11 for this header; C++23 for the optional driver.hpp helper
 *   - sizeof(void*) == 8, sizeof(size_t) == 8, function pointers are
 *     8 bytes, default struct packing, little-endian
 *   - the host loader is dlopen (RTLD_NOW | RTLD_LOCAL), not a Windows loader
 * This header is pure C and may only depend on <stddef.h> and <stdint.h>.
 *
 * The version is abi_major.abi_minor. ABI 1.0 is major 1, minor 0.
 * The first 8 bytes of every versioned struct are the version prefix
 * (struct_size, abi_major, abi_minor). That prefix does not move, including
 * across a major bump, so a host can read it before interpreting the rest.
 * The host is the provider: it loads a plugin when it provides the plugin's
 * declared version, and a plugin accepts a services table when that table
 * provides the version the plugin was compiled against. Same major, and the
 * provider's minor is at least the minor required. A plugin's minor is the
 * minimum host minor it requires.
 *
 * Within a major, fields are only appended to the versioned top-level
 * structs: yaddnsc_error, yaddnsc_http_request, yaddnsc_http_response,
 * yaddnsc_update_request, yaddnsc_driver_descriptor, yaddnsc_host_services.
 * New callbacks, capability bits, status codes, log levels, and HTTP methods
 * are minor bumps. A host accepts every value introduced by its minor and
 * below. Moving, inserting, or removing a field, growing a leaf type, or
 * adding a required entry point is a major bump.
 * Leaf types (yaddnsc_string, yaddnsc_bytes, yaddnsc_source_location,
 * yaddnsc_http_header) have a fixed layout and no version field; do not
 * append fields to them. YADDNSC_*_MIN_SIZE is the ABI 1.0 baseline and
 * stays pointed at that baseline when a later minor appends a field. A
 * struct_size below the baseline is rejected once the version has been
 * accepted. Readers use yaddnsc_struct_has_field before touching a field
 * added after ABI 1.0.
 *
 * Plugins are in-process trusted code, not a sandbox. A plugin can abort,
 * loop forever, corrupt host memory, or call the system directly.
 */

#include <stddef.h>
#include <stdint.h>

/* All supported platforms are little-endian; the ABI defines no big-endian
 * layout. Toolchains that do not define these macros pass silently. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the v1 ABI assumes a little-endian platform"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define YADDNSC_DRIVER_ABI_MAJOR UINT16_C(1)
#define YADDNSC_DRIVER_ABI_MINOR UINT16_C(0)
#define YADDNSC_DRIVER_MAGIC UINT64_C(0x594144444E534300)

/* True when the provider implements every requirement of need. The host is
 * the provider on both sides: it provides the version a plugin declares, and
 * its services table provides the version the plugin was compiled against.
 */
static inline int yaddnsc_abi_provides(uint16_t provider_major, uint16_t provider_minor, uint16_t need_major,
                                       uint16_t need_minor) {
    return provider_major == need_major && provider_minor >= need_minor;
}

/* ── View types ───────────────────────────────────────────────────────────
 * Borrowed views: `data` is not necessarily NUL-terminated, `size` is the
 * only valid length. size == 0 with data == NULL is legal; size > 0 with
 * data == NULL is invalid. Every message view returned by the ABI is
 * borrowed: the caller must copy it before the call returns and must not
 * cache it. Views expire when the call returns unless a stricter lifetime
 * is documented for the specific callback.
 *
 * HTTP request body is the one view with a presence bit:
 *   {NULL, 0}     no body
 *   {non-NULL, 0} a body of length 0
 *   {non-NULL, N} N bytes
 *   {NULL, N}     invalid
 */
typedef struct yaddnsc_string {
    const char* data;
    size_t size;
} yaddnsc_string;

typedef struct yaddnsc_bytes {
    const uint8_t* data;
    size_t size;
} yaddnsc_bytes;

/* A zero-length view may use NULL data. A non-zero length view must point to
 * readable storage. These helpers are the mandatory validation boundary
 * before constructing a C++ string_view/span from an ABI view.
 */
static inline int yaddnsc_string_is_valid(yaddnsc_string value) {
    return value.size == 0 || value.data != NULL;
}

static inline int yaddnsc_bytes_is_valid(yaddnsc_bytes value) {
    return value.size == 0 || value.data != NULL;
}

/* ── Status codes ─────────────────────────────────────────────────────────
 * Closed set for ABI 1.0. A new status is a minor bump; a host accepts every
 * status introduced by its minor and below. An unknown status makes an error
 * report illegal. On YADDNSC_STATUS_OK the callee does not write out_error.
 * On any other status, when out_error is non-NULL and large enough,
 * out_error->status must equal the returned status. out_error == NULL, or
 * a struct_size below the ABI 1.0 baseline, leaves the error struct unchanged;
 * the returned status is still authoritative.
 *
 * retry_after_seconds == 0 means the plugin gives no hint. Any non-OK
 * status may set it. The host applies the hint only when it is greater
 * than 0.
 */
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

static inline int yaddnsc_status_is_valid(yaddnsc_status status) {
    switch (status) {
        case YADDNSC_STATUS_OK:
        case YADDNSC_STATUS_INVALID_ARGUMENT:
        case YADDNSC_STATUS_INVALID_CONFIG:
        case YADDNSC_STATUS_UNSUPPORTED_RECORD:
        case YADDNSC_STATUS_NETWORK_ERROR:
        case YADDNSC_STATUS_AUTHENTICATION_FAILED:
        case YADDNSC_STATUS_RATE_LIMITED:
        case YADDNSC_STATUS_UPSTREAM_REJECTED:
        case YADDNSC_STATUS_INVALID_RESPONSE:
        case YADDNSC_STATUS_CANCELLED:
        case YADDNSC_STATUS_INTERNAL_ERROR:
            return 1;
        default:
            return 0;
    }
}

/* ── Log levels ───────────────────────────────────────────────────────────
 * Closed set for ABI 1.0. A new level is a minor bump. This host records an
 * unknown level as INFO. log() itself never fails an update.
 */
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

/* ── HTTP ─────────────────────────────────────────────────────────────────
 * Methods this host accepts. An unknown method is INVALID_ARGUMENT.
 */
typedef uint32_t yaddnsc_http_method;
#define YADDNSC_HTTP_GET UINT32_C(1)
#define YADDNSC_HTTP_POST UINT32_C(2)
#define YADDNSC_HTTP_PUT UINT32_C(3)
#define YADDNSC_HTTP_DELETE UINT32_C(4)
#define YADDNSC_HTTP_PATCH UINT32_C(5)
#define YADDNSC_HTTP_HEAD UINT32_C(6)
#define YADDNSC_HTTP_OPTIONS UINT32_C(7)

/* Capability bits are a host-enforced contract, not a hint. The host
 * delivers "A" only when CAPABILITY_A is set and "AAAA" only when
 * CAPABILITY_AAAA is set. Any other record type, including "TXT", is
 * rejected before the plugin is called, so that path produces no
 * yaddnsc_status. YADDNSC_STATUS_UNSUPPORTED_RECORD is the status a
 * plugin returns when it refuses a record. Zero is legal at load and
 * means the plugin accepts no ABI 1.0 record type. Unknown bits are
 * rejected at load. ABI 1.0 has no capability bit for any other record
 * type. A new bit is a minor bump; the version check runs first, and a host
 * accepts every bit introduced by its minor and below.
 */
#define YADDNSC_DRIVER_CAPABILITY_A (UINT64_C(1) << 0)
#define YADDNSC_DRIVER_CAPABILITY_AAAA (UINT64_C(1) << 1)
#define YADDNSC_DRIVER_CAPABILITIES_SUPPORTED (YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

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

static inline int yaddnsc_http_header_is_valid(yaddnsc_http_header header) {
    return yaddnsc_string_is_valid(header.name) && yaddnsc_string_is_valid(header.value);
}

static inline int yaddnsc_http_header_array_is_valid(const yaddnsc_http_header* headers, size_t count) {
    return count == 0 || headers != NULL;
}

static inline int yaddnsc_driver_capabilities_are_valid(uint64_t capabilities) {
    return (capabilities & ~YADDNSC_DRIVER_CAPABILITIES_SUPPORTED) == 0;
}

typedef struct yaddnsc_http_request {
    uint32_t struct_size;
    yaddnsc_string url;
    yaddnsc_http_method method;
    const yaddnsc_http_header* headers;
    size_t header_count;
    yaddnsc_string content_type;
    yaddnsc_bytes body;
} yaddnsc_http_request;

typedef struct yaddnsc_http_response {
    uint32_t struct_size;
    uint32_t status_code;
    const yaddnsc_http_header* headers;
    size_t header_count;
    yaddnsc_bytes body;
} yaddnsc_http_response;

/* ip_address: textual address produced by inet_ntop. No zone index and no
 *   brackets. IPv4 is dotted decimal; IPv6 is the compressed inet_ntop form.
 * record_type: "A" or "AAAA", case-sensitive. The host does not deliver any
 *   other token.
 * domain: configured zone name, without a trailing dot.
 * subdomain: configured label, passed through unchanged. "@" is the apex
 *   label; the plugin maps it to the form its provider requires.
 * fqdn: absolute name with the apex already collapsed, without a trailing dot.
 * driver_param_json: host-serialised, valid JSON UTF-8 bytes; owned by the
 *   host, valid for the duration of yaddnsc_driver_update() only.
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
    uint16_t abi_major;
    uint16_t abi_minor;
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
 * is host-owned and opaque: the plugin passes it back verbatim. The table
 * may be saved during create and stays valid until the matching destroy
 * returns. It must not be used after that, and it must not be handed to
 * another thread.
 *
 * Different driver instances may run concurrently. One instance is never
 * used concurrently. Host callbacks do not re-enter plugin entry points.
 *
 * log:           returns void — logging failure must never fail an update.
 * http_exchange: callable only from yaddnsc_driver_update, on that call's
 *                thread. create, destroy, and validate must not call it;
 *                the validate table rejects the call without I/O.
 *                non-OK means transport or cancellation failure only.
 *                Provider HTTP status, including 4xx/5xx and integers
 *                outside 100..599, arrives via out_response->status_code
 *                with STATUS_OK. The body is binary-safe. Response headers
 *                may repeat and keep the peer's casing. Compare names
 *                case-insensitively. Trailers are not exposed. Response views
 *                stay valid until yaddnsc_driver_update() returns (host
 *                arena), across multiple exchanges, and must not be cached
 *                past that return.
 *                Request headers named Host, Content-Length, Content-Type,
 *                Connection, and User-Agent are discarded; names are compared
 *                case-insensitively. The host supplies Host from the URL, one
 *                User-Agent, Content-Length from the body, and Connection
 *                when its policy requires one. Content-Type is taken only
 *                from the content_type field, and only when a body is
 *                present (data != NULL). Transfer-Encoding, Trailer, and
 *                Upgrade are not sent; the exchange fails.
 *                This host allows only the http and https schemes, follows
 *                redirects up to 10 times (301/302/303 become GET and drop
 *                the body; 307/308 keep the method and body; an https to
 *                http downgrade is not followed), and limits one exchange
 *                to 64 KiB of headers and 16 MiB of body. The plugin sees
 *                the final response.
 * is_cancelled:  int32_t; 0 = the current update operation is active,
 *                non-0 = its cancellation token was triggered. A predicate,
 *                not a fallible operation. A 0 result does not promise that
 *                the next http_exchange avoids CANCELLED: cancellation can
 *                happen between the predicate and the exchange. If the
 *                operation is already cancelled when an exchange starts, the
 *                host does not send the request. A plugin that receives
 *                CANCELLED stops the current update.
 */
struct yaddnsc_host_services {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    void* context;
    void (*log)(void* context, yaddnsc_log_level level, yaddnsc_string message,
                const yaddnsc_source_location* location);
    yaddnsc_status (*http_exchange)(void* context, const yaddnsc_http_request* request,
                                    yaddnsc_http_response* out_response, yaddnsc_error* out_error);
    int32_t (*is_cancelled)(void* context);
};

/* ── Exported entry points (extern "C", no exceptions, no C++ objects) ────
 *
 * Four entry points are REQUIRED: get_descriptor, create, destroy, update.
 * A fifth, yaddnsc_driver_validate, is OPTIONAL since ABI 1.0: the
 * host resolves it with dlsym. When it is absent, the plugin still loads
 * and can update, and config test fails because the host cannot confirm
 * driver_param. YADDNSC_DEFINE_DRIVER always exports the entry. The C++
 * helper's default Driver::validate() returns success, so config test
 * passes and no schema check runs. Override it to check the schema.
 *
 * No entry point may let a C++ exception cross the ABI. The yaddnsc host
 * also catches exceptions as a backstop; another host may not. The backstop
 * assumes the plugin shares an ABI-compatible C++ runtime with the host —
 * an escape from a plugin built with an incompatible runtime is undefined
 * before any catch runs. The SDK entry points forward to noexcept helpers
 * that catch everything, so a plugin built with YADDNSC_DEFINE_DRIVER never
 * relies on the backstop.
 */

yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out_descriptor);

/* Handle ownership: on YADDNSC_STATUS_OK, *out_driver is non-NULL, the host
 * owns the instance, and the host guarantees the matching destroy() call.
 * OK with a NULL handle is a contract violation; the host reports
 * INTERNAL_ERROR and owns nothing. On ANY non-OK return the plugin must
 * leave *out_driver NULL — a failed create owns nothing and must have
 * released its state already. The host clears *out_driver before the call.
 * If a plugin stores a handle and then reports failure, the host destroys
 * and clears that handle.
 */
yaddnsc_status yaddnsc_driver_create(const yaddnsc_host_services* services, yaddnsc_driver** out_driver,
                                     yaddnsc_error* out_error);

void yaddnsc_driver_destroy(yaddnsc_driver* driver);

yaddnsc_status yaddnsc_driver_update(yaddnsc_driver* driver, const yaddnsc_update_request* request,
                                     yaddnsc_error* out_error);

/* OPTIONAL — validate driver_param without an update. A plugin that does
 * not export this symbol is not promising that its configuration is valid.
 *
 * Called by the host's `config test` on a live instance between
 * yaddnsc_driver_create() and yaddnsc_driver_destroy(); `driver` is exactly
 * the handle create() produced. `driver_param_json` is host-serialised,
 * valid JSON UTF-8 bytes, owned by the host and valid for the duration of
 * the call only.
 *
 * Return YADDNSC_STATUS_OK when the parameter is acceptable, or
 * YADDNSC_STATUS_INVALID_CONFIG with a human-readable message when it
 * violates the driver's schema; any other non-OK status is treated as a
 * validation failure as well. Implementations must not depend on host
 * services (no HTTP exchange is available during validation) and must be
 * prepared for concurrent calls on distinct instances.
 */
yaddnsc_status yaddnsc_driver_validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                       yaddnsc_error* out_error);

/* ── struct_size helpers (C and C++) ──────────────────────────────────────*/

/* Byte size covering `member` of `type` completely — the only legal test
 * for field visibility. All arithmetic is compile-time constant. */
#define YADDNSC_SIZEOF_THROUGH(type, member) ((uint32_t) (offsetof(type, member) + sizeof(((type*) 0)->member)))

/* True when `field_end` (YADDNSC_SIZEOF_THROUGH of the field) is fully
 * covered by the supplied struct_size. */
static inline int yaddnsc_struct_has_field(uint32_t struct_size, uint32_t field_end) {
    return struct_size >= field_end;
}

/* ABI 1.0 baseline. Do not retarget these macros when a later minor appends
 * a field: a 1.0 peer's struct_size stops here, and a newer peer must still
 * accept it. Gate every later field with yaddnsc_struct_has_field. */
#define YADDNSC_ERROR_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_error, message)
#define YADDNSC_HTTP_REQUEST_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_http_request, body)
#define YADDNSC_HTTP_RESPONSE_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_http_response, body)
#define YADDNSC_UPDATE_REQUEST_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_update_request, driver_param_json)
#define YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_driver_descriptor, capabilities)
#define YADDNSC_HOST_SERVICES_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_host_services, is_cancelled)

/* Bytes required to read abi_major and abi_minor. This prefix does not move. */
#define YADDNSC_ABI_VERSION_PREFIX_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_driver_descriptor, abi_minor)

/* ── ABI assertions ───────────────────────────────────────────────────────
 * Pin everything an accidental edit could silently break: constant numeric
 * values (a reassigned status, method, or capability bit corrupts every
 * deployed plugin), field offsets, the ABI 1.0 baselines, leaf-type sizes,
 * and alignment. A deliberate minor bump updates only the pins that say so;
 * see docs/development.md#plugin-abi-changes.
 */
#define YADDNSC_ABI_ASSERT_OFFSET(type, member, expected)                                       \
    YADDNSC_STATIC_ASSERT(offsetof(type, member) == (expected), #type "." #member               \
                                                                      " offset changed — bump " \
                                                                      "abi_major")

#if defined(__cplusplus)
#define YADDNSC_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define YADDNSC_ALIGNOF(type) alignof(type)
#else
#define YADDNSC_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define YADDNSC_ALIGNOF(type) _Alignof(type)
#endif

/* Platform assumptions. */
YADDNSC_STATIC_ASSERT(sizeof(void*) == 8, "the v1 ABI assumes a 64-bit platform");
YADDNSC_STATIC_ASSERT(sizeof(size_t) == 8, "the v1 ABI assumes 64-bit size_t");
YADDNSC_STATIC_ASSERT(sizeof(void (*)(void)) == 8, "the v1 ABI assumes 64-bit function pointers");

/* Version and magic. */
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_ABI_MAJOR == 1, "ABI major changed — a new major is a new ABI, not an edit");
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_ABI_MINOR == 0,
                      "ABI minor bump: update this pin deliberately (docs/development.md#plugin-abi-changes)");
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_MAGIC == UINT64_C(0x594144444E534300), "driver magic changed");

/* Status codes — a closed set. A new code is a minor bump; changing a value
 * is an ABI break. */
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_OK == 0, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_INVALID_ARGUMENT == 1, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_INVALID_CONFIG == 2, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_UNSUPPORTED_RECORD == 3, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_NETWORK_ERROR == 4, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_AUTHENTICATION_FAILED == 5, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_RATE_LIMITED == 6, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_UPSTREAM_REJECTED == 7, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_INVALID_RESPONSE == 8, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_CANCELLED == 9, "status value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_INTERNAL_ERROR == 10, "status value changed");

/* Log levels — a closed set. */
YADDNSC_STATIC_ASSERT(YADDNSC_LOG_TRACE == 0, "log level value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_LOG_DEBUG == 1, "log level value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_LOG_INFO == 2, "log level value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_LOG_WARN == 3, "log level value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_LOG_ERROR == 4, "log level value changed");

/* HTTP methods — a closed set. */
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_GET == 1, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_POST == 2, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_PUT == 3, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_DELETE == 4, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_PATCH == 5, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_HEAD == 6, "http method value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_OPTIONS == 7, "http method value changed");

/* Capability bits. */
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_CAPABILITY_A == (UINT64_C(1) << 0), "capability bit changed");
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_CAPABILITY_AAAA == (UINT64_C(1) << 1), "capability bit changed");
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_CAPABILITIES_SUPPORTED == UINT64_C(3), "capability mask changed");

/* ABI 1.0 baselines — frozen forever. A later minor appends fields, but
 * these stay pointed at the 1.0 layout. */
YADDNSC_STATIC_ASSERT(YADDNSC_ABI_VERSION_PREFIX_SIZE == 8, "ABI version prefix moved");
YADDNSC_STATIC_ASSERT(YADDNSC_ERROR_MIN_SIZE == 32, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_REQUEST_MIN_SIZE == 80, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_HTTP_RESPONSE_MIN_SIZE == 40, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_UPDATE_REQUEST_MIN_SIZE == 104, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_DESCRIPTOR_MIN_SIZE == 88, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_HOST_SERVICES_MIN_SIZE == 40, "ABI 1.0 baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_SIZEOF_THROUGH(yaddnsc_host_services, abi_minor) == YADDNSC_ABI_VERSION_PREFIX_SIZE,
                      "host services version prefix diverges from the descriptor");

/* Leaf types have a fixed layout and no version field: sizes and alignment
 * never change. */
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_string) == 16, "yaddnsc_string layout changed");
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_bytes) == 16, "yaddnsc_bytes layout changed");
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_source_location) == 40, "yaddnsc_source_location layout changed");
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_http_header) == 32, "yaddnsc_http_header layout changed");

/* Alignment — nothing in the v1 type vocabulary needs more than 8. A type
 * with a larger alignment would be a new ABI. */
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_string) == 8, "yaddnsc_string alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_bytes) == 8, "yaddnsc_bytes alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_source_location) == 8, "yaddnsc_source_location alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_error) == 8, "yaddnsc_error alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_http_header) == 8, "yaddnsc_http_header alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_http_request) == 8, "yaddnsc_http_request alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_http_response) == 8, "yaddnsc_http_response alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_update_request) == 8, "yaddnsc_update_request alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_driver_descriptor) == 8, "yaddnsc_driver_descriptor alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_host_services) == 8, "yaddnsc_host_services alignment changed");

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
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, abi_major, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, abi_minor, 6);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, magic, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, name, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, version, 32);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, author, 48);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, description, 64);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_driver_descriptor, capabilities, 80);

YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, abi_major, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, abi_minor, 6);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, context, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, log, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, http_exchange, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services, is_cancelled, 32);

#undef YADDNSC_ABI_ASSERT_OFFSET
#undef YADDNSC_STATIC_ASSERT
#undef YADDNSC_ALIGNOF

#ifdef __cplusplus
}
#endif

#endif /* YADDNSC_SDK_DRIVER_ABI_H */
