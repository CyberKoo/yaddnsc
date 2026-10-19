/*
 * C11 compile smoke test for include/yaddnsc/sdk/driver_abi.h.
 * The build compiles this file as C11 with -Wall -Wextra -Wpedantic -Werror.
 * It is not linked.
 */

#include <yaddnsc/sdk/driver_abi.h>

static void log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                      const yaddnsc_source_location* location) {
    (void) context;
    (void) level;
    (void) message;
    (void) location;
}

static yaddnsc_status exchange_entry(void* context, const yaddnsc_http_request* request,
                                     yaddnsc_http_response* out_response, yaddnsc_error* out_error) {
    (void) context;
    (void) request;
    (void) out_response;
    (void) out_error;
    return YADDNSC_STATUS_OK;
}

static int32_t cancelled_entry(void* context) {
    (void) context;
    return 0;
}

static yaddnsc_status async_http_start(void* context, const yaddnsc_http_request* request, uint64_t* out_op_id,
                                       yaddnsc_error* out_error) {
    (void) context;
    (void) request;
    (void) out_error;
    if (out_op_id != NULL) {
        *out_op_id = UINT64_C(1);
    }
    return YADDNSC_STATUS_OK;
}

static yaddnsc_status async_http_cancel(void* context, uint64_t op_id, yaddnsc_error* out_error) {
    (void) context;
    (void) op_id;
    (void) out_error;
    return YADDNSC_STATUS_OK;
}

static int32_t async_log_enabled(void* context, yaddnsc_log_level level) {
    (void) context;
    (void) level;
    return 1;
}

static yaddnsc_status describe(const yaddnsc_driver_descriptor** out_descriptor) {
    static const yaddnsc_driver_descriptor descriptor = {
        .struct_size = sizeof(yaddnsc_driver_descriptor),
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
        .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
        .magic = YADDNSC_DRIVER_MAGIC,
        .name = {"c_abi", sizeof("c_abi") - 1},
        .version = {"0", sizeof("0") - 1},
        .author = {"yaddnsc", sizeof("yaddnsc") - 1},
        .description = {"c11", sizeof("c11") - 1},
        .capabilities = YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA,
    };
    if (out_descriptor == NULL) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_descriptor = &descriptor;
    return YADDNSC_STATUS_OK;
}

/* External linkage so an unreferenced smoke function is not a warning. */
int yaddnsc_c_abi_compile_smoke(void) {
    const yaddnsc_string text = {"a", 1};
    const uint8_t byte = 0;
    const yaddnsc_bytes empty_body = {&byte, 0};
    const yaddnsc_bytes absent_body = {NULL, 0};
    const yaddnsc_source_location location = {.file = text, .line = 1, .function = text};
    const yaddnsc_http_header header = {.name = text, .value = text};
    const yaddnsc_error error = {
        .struct_size = sizeof(yaddnsc_error),
        .status = YADDNSC_STATUS_OK,
        .retry_after_seconds = 0,
        .message = text,
    };
    const yaddnsc_http_request request = {
        .struct_size = sizeof(yaddnsc_http_request),
        .url = text,
        .method = YADDNSC_HTTP_GET,
        .headers = &header,
        .header_count = 1,
        .content_type = text,
        .body = empty_body,
    };
    const yaddnsc_http_response response = {
        .struct_size = sizeof(yaddnsc_http_response),
        .status_code = 204,
        .headers = &header,
        .header_count = 1,
        .body = absent_body,
    };
    const yaddnsc_update_request update = {
        .struct_size = sizeof(yaddnsc_update_request),
        .ip_address = text,
        .record_type = text,
        .domain = text,
        .subdomain = text,
        .fqdn = text,
        .driver_param_json = absent_body,
    };
    const yaddnsc_host_services services = {
        .struct_size = sizeof(yaddnsc_host_services),
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
        .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
        .context = NULL,
        .log = log_entry,
        .http_exchange = exchange_entry,
        .is_cancelled = cancelled_entry,
    };
    const yaddnsc_host_services_v1_1 extended_services = {
        .base = services,
        .log_enabled = NULL,
    };
    const yaddnsc_async_services async_services = {
        .struct_size = sizeof(yaddnsc_async_services),
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
        .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
        .context = NULL,
        .log = log_entry,
        .is_cancelled = cancelled_entry,
        .http_start = async_http_start,
        .http_cancel = async_http_cancel,
        .log_enabled = async_log_enabled,
    };
    const yaddnsc_event event = {
        .struct_size = sizeof(yaddnsc_event),
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
        .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
        .kind = YADDNSC_EVENT_HTTP_FINISHED,
        .op_id = UINT64_C(1),
        .status = YADDNSC_STATUS_OK,
        .response = &response,
        .error = NULL,
    };
    const yaddnsc_async_driver* async_handle = NULL;
    const yaddnsc_status async_step = YADDNSC_STATUS_PENDING;

    yaddnsc_status (*const get_descriptor)(const yaddnsc_driver_descriptor**) = yaddnsc_driver_get_descriptor;
    yaddnsc_status (*const create)(const yaddnsc_host_services*, yaddnsc_driver**, yaddnsc_error*) =
        yaddnsc_driver_create;
    void (*const destroy)(yaddnsc_driver*) = yaddnsc_driver_destroy;
    yaddnsc_status (*const update_entry)(yaddnsc_driver*, const yaddnsc_update_request*, yaddnsc_error*) =
        yaddnsc_driver_update;
    yaddnsc_status (*const validate)(yaddnsc_driver*, yaddnsc_string, yaddnsc_error*) = yaddnsc_driver_validate;
    yaddnsc_status (*const async_create)(const yaddnsc_async_services*, yaddnsc_async_driver**, yaddnsc_error*) =
        yaddnsc_driver_async_create;
    void (*const async_destroy)(yaddnsc_async_driver*) = yaddnsc_driver_async_destroy;
    yaddnsc_status (*const async_begin)(yaddnsc_async_driver*, const yaddnsc_update_request*, yaddnsc_error*) =
        yaddnsc_driver_update_async_begin;
    yaddnsc_status (*const async_resume)(yaddnsc_async_driver*, const yaddnsc_event*, yaddnsc_error*) =
        yaddnsc_driver_update_async_resume;
    yaddnsc_status (*const async_validate)(yaddnsc_async_driver*, yaddnsc_string, yaddnsc_error*) =
        yaddnsc_driver_async_validate;

    const yaddnsc_driver_descriptor* described = NULL;
    return yaddnsc_string_is_valid(text) + yaddnsc_bytes_is_valid(empty_body) + yaddnsc_bytes_is_valid(absent_body) +
           yaddnsc_status_is_valid(error.status) + yaddnsc_http_header_is_valid(header) +
           yaddnsc_http_header_array_is_valid(request.headers, request.header_count) +
           yaddnsc_driver_capabilities_are_valid(YADDNSC_DRIVER_CAPABILITIES_SUPPORTED) +
           yaddnsc_struct_has_field(update.struct_size, YADDNSC_UPDATE_REQUEST_MIN_SIZE) + (location.line > 0) +
           (response.status_code == 204) + (services.is_cancelled != NULL) + (extended_services.log_enabled == NULL) +
           (async_services.http_start != NULL) +
           (yaddnsc_struct_has_field(async_services.struct_size, YADDNSC_ASYNC_SERVICES_MIN_SIZE)) +
           (event.kind == YADDNSC_EVENT_HTTP_FINISHED) + (async_handle == NULL) +
           (yaddnsc_async_step_status_is_valid(async_step)) +
           (get_descriptor != NULL) +
           (create != NULL) + (destroy != NULL) + (update_entry != NULL) + (validate != NULL) +
           (async_create != NULL) + (async_destroy != NULL) + (async_begin != NULL) + (async_resume != NULL) +
           (async_validate != NULL) + (int)describe(&described);
}
