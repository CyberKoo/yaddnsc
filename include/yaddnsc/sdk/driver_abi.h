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
 * Only the descriptor and Host Services begin with the 8-byte version prefix
 * (struct_size, abi_major, abi_minor). That prefix does not move, including
 * across a major bump, so a host can read it before interpreting the rest.
 * The host is the provider: it loads a plugin when it provides the plugin's
 * declared version, and a plugin accepts a services table when that table
 * provides the minimum version the plugin requires. Same major, and the
 * provider's minor is at least the minor required. A plugin's minor is the
 * minimum host minor it requires.
 *
 * Within a major, fields are only appended to the extensible top-level
 * structs: yaddnsc_error, yaddnsc_http_request, yaddnsc_http_response,
 * yaddnsc_update_request, yaddnsc_driver_descriptor, yaddnsc_host_services,
 * and — from ABI 1.1 — yaddnsc_host_services_v1_1, yaddnsc_async_services and
 * yaddnsc_event.
 * New callbacks, capability bits, status codes, log levels, and HTTP methods
 * are minor bumps. An optional host-services field may instead be added as a
 * new container type whose first member is the unchanged previous table
 * (yaddnsc_host_services_v1_1); the provider passes &base to the same entry
 * point and base.struct_size covers the whole container. This keeps the 1.0
 * type and its MIN_SIZE unchanged for existing source. A host accepts every
 * value introduced by its minor and below. Moving, inserting, or removing a
 * field, growing a leaf type, or adding a required entry point is a major bump.
 * Leaf types (yaddnsc_string, yaddnsc_bytes, yaddnsc_source_location,
 * yaddnsc_http_header) have a fixed layout and no version field; do not
 * append fields to them. The ABI 1.0 YADDNSC_*_MIN_SIZE macros stay pointed
 * at that baseline when a later minor appends a field; a new container
 * introduced by a later minor gets its own MIN_SIZE. A
 * struct_size below the baseline is rejected for required inputs/outputs;
 * optional out_error below the baseline is left unchanged. The enclosing
 * protocol supplies the version for structures without major/minor fields.
 * Readers use yaddnsc_struct_has_field before touching a field added after
 * ABI 1.0.
 *
 * Layout convention: a validation inline is placed next to the type it
 * validates, under that type's own banner. A type introduced by a later minor
 * brings its validator with it rather than collecting validators in one
 * block, so the frozen declaration order below only ever grows.
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
#define YADDNSC_DRIVER_ABI_MINOR UINT16_C(1)
#define YADDNSC_DRIVER_MAGIC UINT64_C(0x594144444E534300)

/* ── Version negotiation ──────────────────────────────────────────────────
 * True when the provider implements every requirement of need. The host is
 * the provider on both sides: it provides the version a plugin declares, and
 * its services table provides the minimum version the plugin requires.
 */
static inline int yaddnsc_abi_provides(uint16_t provider_major, uint16_t provider_minor, uint16_t need_major,
                                       uint16_t need_minor) {
    return provider_major == need_major && provider_minor >= need_minor;
}

/* ── View types ───────────────────────────────────────────────────────────
 * Borrowed views: `data` is not necessarily NUL-terminated, `size` is the
 * only valid length. size == 0 with data == NULL is legal; size > 0 with
 * data == NULL is invalid. All views borrow storage:
 * - Input views and log arguments are valid only during the receiving call;
 *   the receiver must copy them before returning if it needs to retain them.
 * - Returned error messages are valid after return until the next ABI entry
 *   or service callback on the same thread, instance destruction, or module
 *   unload, whichever happens first. The caller copies them before that event.
 * - HTTP responses remain valid until the current update returns, across
 *   exchanges. Descriptor views remain valid while the module is loaded.
 * - Services and context may be used from create through matching destroy
 *   on that cycle's worker; they must never be retained across cycles.
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

/* ── View validation ──────────────────────────────────────────────────────
 * A zero-length view may use NULL data. A non-zero length view must point to
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
 * Completion set for ABI 1.0; ABI 1.1 adds YADDNSC_STATUS_PENDING for async
 * step results only. A new status is a minor bump; a host accepts every
 * status introduced by its minor and below. An unknown status makes an error
 * report illegal. PENDING is never written to out_error and is not accepted
 * by the ABI 1.0 completion entries. On YADDNSC_STATUS_OK the callee does not
 * write out_error.
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

/* ABI 1.1: a step result, not a completion status. Only async begin/resume
 * may return it; create, validate, http_start and http_cancel must not. */
#define YADDNSC_STATUS_PENDING UINT32_C(11)

/* ── Status validation ────────────────────────────────────────────────────
 * An unknown status makes an error report illegal; the host records one as
 * INTERNAL_ERROR and a plugin must not produce one.
 */
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

/* ── Step-status validation ───────────────────────────────────────────────
 * The async step set: the 1.0 completion set plus PENDING. Use this for
 * begin/resume results only; keep yaddnsc_status_is_valid for old entries.
 */
static inline int yaddnsc_async_step_status_is_valid(yaddnsc_status status) {
    return yaddnsc_status_is_valid(status) || status == YADDNSC_STATUS_PENDING;
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

/* ── Driver capability bits ───────────────────────────────────────────────
 * Capability bits are a host-enforced contract, not a hint. The host
 * delivers "A" only when CAPABILITY_A is set and "AAAA" only when
 * CAPABILITY_AAAA is set. Any other record type, including "TXT", is
 * rejected before the plugin is called, so that path produces no
 * yaddnsc_status. YADDNSC_STATUS_UNSUPPORTED_RECORD is the status a
 * plugin returns when it refuses a record. Zero is legal at load and
 * means the plugin accepts no ABI 1.0 record type. Unknown bits are
 * rejected at load. ABI 1.0 has no capability bit for any other record
 * type. A new bit is a minor bump; the version check runs first, and a host
 * accepts every bit introduced by its minor and below.
 *
 * The capability validator lives below, with the other inline validators
 * kept in their frozen positions.
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

/* ── Header validation ────────────────────────────────────────────────────
 * A header array is valid when it is empty or points at header_count
 * readable headers, and each header's two views are valid.
 */
static inline int yaddnsc_http_header_is_valid(yaddnsc_http_header header) {
    return yaddnsc_string_is_valid(header.name) && yaddnsc_string_is_valid(header.value);
}

static inline int yaddnsc_http_header_array_is_valid(const yaddnsc_http_header* headers, size_t count) {
    return count == 0 || headers != NULL;
}

/* ── Capability validation ────────────────────────────────────────────────
 * The load-time gate for YADDNSC_DRIVER_CAPABILITY_*: no bit outside
 * YADDNSC_DRIVER_CAPABILITIES_SUPPORTED may be set.
 */
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

/* ABI 1.1 — optional log-level query.
 *
 * The provider allocates this full container and passes `&base` to the
 * unchanged ABI 1.0 create() entry point. `base.struct_size` therefore covers
 * the whole container and `base.abi_minor` is at least 1. A plugin must check
 * the version and `yaddnsc_struct_has_field(base.struct_size,
 * YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE)` before reading `log_enabled`.
 *
 * log_enabled returns 0 when the level is disabled and non-zero when it is
 * enabled. It is an early filter only: the plugin may still call log, and the
 * host performs the final level check before emitting. A NULL field means the
 * host provides no early answer; the plugin keeps the original unconditional
 * log path. Never read this field from a copy typed as the 1.0 table; copy the
 * 1.0 prefix first and use the SDK's validated accessor.
 */
typedef struct yaddnsc_host_services_v1_1 {
    yaddnsc_host_services base;
    /* 0 = disabled, non-zero = enabled; optional (may be NULL). */
    int32_t (*log_enabled)(void* context, yaddnsc_log_level level);
} yaddnsc_host_services_v1_1;

/* ── ABI 1.1 async execution ───────────────────────────────────────────────
 *
 * Async mode uses a separate services table and a separate entry-point group.
 * The only export common to both modes is get_descriptor. A plugin that
 * adopts the new negotiation rules uses exactly one complete group and must
 * not export a symbol from the other group. The min-0 case below is the
 * legacy compatibility exception. The mode is fixed at load. Selection rules:
 * - a plugin whose descriptor requires minor 0 and exports a complete sync
 *   group stays sync, even if it also exports async names; the host keeps it
 *   on the sync path and never supplies the async table;
 * - async mode requires a descriptor minimum minor of at least 1 and a
 *   complete async group;
 * - a plugin requiring minor 1 or newer may still choose the sync group; the
 *   declared minimum alone does not select async mode;
 * - a plugin requiring minor 1 or newer must export exactly one complete
 *   group, including the matching validate entry and no symbol from the other
 *   group, or it is rejected.
 * A failed async begin/resume never falls back to sync update, and a failed
 * sync entry never falls back to async. The async table never carries the
 * sync http_exchange callback, and neither table exposes a conversion to the
 * other.
 *
 * Lifecycle: create → begin → [PENDING → resume]* → OK/error → destroy;
 * create → validate → destroy; create → destroy when cancelled before begin.
 * An instance serves one update or one standalone no-network validate and is
 * not reused across cycles. Calls on one instance are serial; distinct
 * instances may run concurrently without thread affinity.
 *
 * begin and resume return OK, PENDING or an existing completion status.
 * PENDING requires at least one accepted, undelivered op (a queued/running
 * HTTP request or a ready completion). A PENDING with no wake source is a
 * contract violation. create, validate, http_start and http_cancel must not
 * return PENDING. Events are delivered at most once per op; the order follows
 * host completion, not network wall time.
 */
typedef struct yaddnsc_async_driver yaddnsc_async_driver;

/* ABI 1.1 async services. Same version prefix as the sync table; a 1.1
 * provider sets struct_size to sizeof(yaddnsc_async_services) and
 * abi_minor >= 1. The table and context are valid from async_create through
 * async_destroy and may be used only during host-called entry points.
 *
 * http_start: callable only during begin/resume. It accepts a command and
 *   returns a non-zero op_id; the host may send the request only after the
 *   step returns and the owner confirms the update continues. A request
 *   accepted in a terminal step is not sent. On failure no op is created and
 *   no event follows.
 * http_cancel: callable only during begin/resume. Cancels an undelivered op
 *   in the same cycle. Repeated cancels are idempotent; 0, unknown,
 *   already-delivered and currently DELIVERING IDs are rejected. A start and
 *   cancel in the same step fold into a CANCELLED completion without
 *   sending. A READY completion is not overwritten by a later cancel.
 * log and is_cancelled are required non-NULL. log_enabled is optional (may be
 *   NULL); it has the same 0/non-zero early-filter contract as
 *   yaddnsc_host_services_v1_1.
 *
 * The host copies an accepted request before the callback returns. Response
 * and error views carried by an event are valid only for that resume call;
 * the plugin or SDK copies what it retains.
 */
typedef struct yaddnsc_async_services {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    void* context;
    void (*log)(void* context, yaddnsc_log_level level, yaddnsc_string message,
                const yaddnsc_source_location* location);
    int32_t (*is_cancelled)(void* context);
    yaddnsc_status (*http_start)(void* context, const yaddnsc_http_request* request,
                                 uint64_t* out_op_id, yaddnsc_error* out_error);
    yaddnsc_status (*http_cancel)(void* context, uint64_t op_id, yaddnsc_error* out_error);
    int32_t (*log_enabled)(void* context, yaddnsc_log_level level);
} yaddnsc_async_services;

/* One host-to-plugin completion event. The plugin must validate struct_size,
 * abi_major/abi_minor, kind, status and op_id before using the payload.
 * Unknown kinds, unknown statuses, a missing required payload or an unknown
 * op_id must not resume plugin business logic.
 *
 * HTTP_FINISHED: op_id != 0. OK => response non-NULL, error NULL. Any other
 *   valid completion status => error non-NULL with a matching status,
 *   response NULL.
 * UPDATE_CANCELLED: op_id == 0, status CANCELLED, error non-NULL,
 *   response NULL.
 *
 * The event, response, error and their views are host-owned and valid only
 * for the resume call that received them. A future host minor may append
 * fields; it must not use the two padding gaps as field slots.
 */
typedef uint32_t yaddnsc_event_kind;
#define YADDNSC_EVENT_HTTP_FINISHED UINT32_C(1)
#define YADDNSC_EVENT_UPDATE_CANCELLED UINT32_C(2)

typedef struct yaddnsc_event {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    yaddnsc_event_kind kind;
    uint64_t op_id;
    yaddnsc_status status;
    const yaddnsc_http_response* response;
    const yaddnsc_error* error;
} yaddnsc_event;

/* ── Exported entry points (extern "C", no exceptions, no C++ objects) ────
 *
 * The only export common to both modes is get_descriptor. In sync mode a
 * plugin must also export create, destroy and update. A fifth,
 * yaddnsc_driver_validate, is OPTIONAL since ABI 1.0: the host resolves it
 * with dlsym. When it is absent, the plugin still loads and can update, and
 * config test fails because the host cannot confirm driver_param.
 * YADDNSC_DEFINE_DRIVER always exports the entry. The C++ helper's
 * Driver::validate() is pure virtual: plugins must explicitly check their
 * schema and semantics (or accept parameterless configuration).
 *
 * Async mode instead resolves async_create, async_destroy,
 * update_async_begin and update_async_resume as one required group;
 * async_validate is OPTIONAL. Those symbols are conditional requirements of
 * async mode, never common required symbols of every ABI 1.x plugin.
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

/* ── ABI 1.1 async entry points ────────────────────────────────────────────
 *
 * These symbols are REQUIRED only when the host selects async mode. Handle
 * ownership mirrors the sync entries: on OK, *out_driver is non-NULL and the
 * host owns the matching async_destroy; on any failure the plugin leaves it
 * NULL and owns nothing. The host clears *out_driver before the call.
 *
 * async_validate is OPTIONAL. It is a standalone no-network check on a live
 * instance between async_create and async_destroy; absence means the host
 * cannot confirm driver_param, not that the plugin failed to load. Async
 * create/validate must not start HTTP. Only begin/resume may use HTTP
 * callbacks.
 *
 * out_error obeys the same caller-capacity rules as the sync entries. OK and
 * PENDING do not write it. A plugin-written message must stay valid until the
 * host copies it immediately after the entry returns; the host copies on the
 * same worker before the next entry, async_destroy or a worker switch. HTTP
 * callback errors use the step's fixed report buffer, valid until the next
 * HTTP callback or the end of the current entry; log, log_enabled and
 * is_cancelled do not invalidate it.
 */
yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services* services,
                                           yaddnsc_async_driver** out_driver, yaddnsc_error* out_error);

void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver);

yaddnsc_status yaddnsc_driver_update_async_begin(yaddnsc_async_driver* driver,
                                                 const yaddnsc_update_request* request,
                                                 yaddnsc_error* out_error);

yaddnsc_status yaddnsc_driver_update_async_resume(yaddnsc_async_driver* driver, const yaddnsc_event* event,
                                                  yaddnsc_error* out_error);

yaddnsc_status yaddnsc_driver_async_validate(yaddnsc_async_driver* driver, yaddnsc_string driver_param_json,
                                             yaddnsc_error* out_error);

/* ── struct_size helpers and validation (C and C++) ───────────────────────*/

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
/* ABI 1.1 extension baseline; not the 1.0 baseline. */
#define YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE \
    YADDNSC_SIZEOF_THROUGH(yaddnsc_host_services_v1_1, log_enabled)
/* ABI 1.1 async baselines; new types, not retargets of the 1.0 ones. */
#define YADDNSC_ASYNC_SERVICES_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_async_services, log_enabled)
#define YADDNSC_EVENT_MIN_SIZE YADDNSC_SIZEOF_THROUGH(yaddnsc_event, error)

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
YADDNSC_STATIC_ASSERT(YADDNSC_DRIVER_ABI_MINOR == 1,
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
YADDNSC_STATIC_ASSERT(YADDNSC_STATUS_PENDING == 11, "status value changed");

/* ABI 1.1 async event kinds. */
YADDNSC_STATIC_ASSERT(YADDNSC_EVENT_HTTP_FINISHED == 1, "event kind value changed");
YADDNSC_STATIC_ASSERT(YADDNSC_EVENT_UPDATE_CANCELLED == 2, "event kind value changed");

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

/* ABI 1.1 host-services extension: the old table is the first member, and the
 * new field starts at the old table's sizeof. */
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_host_services_v1_1) == 48, "ABI 1.1 host services layout changed");
YADDNSC_STATIC_ASSERT(YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE == 48, "ABI 1.1 host services baseline moved");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_host_services_v1_1) == 8,
                      "ABI 1.1 host services alignment changed");
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services_v1_1, base, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_host_services_v1_1, log_enabled, 40);

/* ABI 1.1 async types. The event type deliberately keeps two padding gaps;
 * they are not field slots for a future minor. */
YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_async_services) == 56, "ABI 1.1 async services layout changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ASYNC_SERVICES_MIN_SIZE == 56, "ABI 1.1 async services baseline moved");
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, abi_major, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, abi_minor, 6);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, context, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, log, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, is_cancelled, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, http_start, 32);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, http_cancel, 40);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_async_services, log_enabled, 48);

YADDNSC_STATIC_ASSERT(sizeof(yaddnsc_event) == 48, "ABI 1.1 event layout changed");
YADDNSC_STATIC_ASSERT(YADDNSC_EVENT_MIN_SIZE == 48, "ABI 1.1 event baseline moved");
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, struct_size, 0);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, abi_major, 4);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, abi_minor, 6);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, kind, 8);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, op_id, 16);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, status, 24);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, response, 32);
YADDNSC_ABI_ASSERT_OFFSET(yaddnsc_event, error, 40);

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
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_async_services) == 8, "yaddnsc_async_services alignment changed");
YADDNSC_STATIC_ASSERT(YADDNSC_ALIGNOF(yaddnsc_event) == 8, "yaddnsc_event alignment changed");

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
