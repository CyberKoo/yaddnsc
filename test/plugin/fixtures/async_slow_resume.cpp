/// Two concurrent exchanges with a worker-side resume rendezvous. The test
/// releases the worker only after the other exchange has observed cancellation.

#include <atomic>
#include <cstdint>
#include <new>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

namespace {

constexpr yaddnsc_driver_descriptor DESCRIPTOR{
    .struct_size = sizeof(yaddnsc_driver_descriptor),
    .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
    .abi_minor = 1,
    .magic = YADDNSC_DRIVER_MAGIC,
    .name = {"async_slow_resume", sizeof("async_slow_resume") - 1},
    .version = {"1.0.0", sizeof("1.0.0") - 1},
    .author = {"yaddnsc", sizeof("yaddnsc") - 1},
    .description = {"Async cancellation rendezvous", sizeof("Async cancellation rendezvous") - 1},
    .capabilities = YADDNSC_DRIVER_CAPABILITY_A,
};

struct State {
    const yaddnsc_async_services* services;
    uint64_t first = 0;
    uint64_t second = 0;
};

void* notification_context = nullptr;
void (*notify_entered)(void*) = nullptr;
std::atomic<bool> released{false};
std::atomic<bool> returned{false};
std::atomic<bool> saw_cancel{false};
std::atomic<unsigned> destroys{0};

void write_error(yaddnsc_error* error, yaddnsc_status status) noexcept {
    if (error != nullptr && error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
        error->struct_size = YADDNSC_ERROR_MIN_SIZE;
        error->status = status;
        error->retry_after_seconds = 0;
        error->message = {};
    }
}

}  // namespace

extern "C" FIXTURE_EXPORT void async_slow_resume_configure(void* context, void (*notify)(void*)) {
    notification_context = context;
    notify_entered = notify;
    released.store(false);
    returned.store(false);
    saw_cancel.store(false);
    destroys.store(0);
}

extern "C" FIXTURE_EXPORT void async_slow_resume_release() {
    released.store(true);
    released.notify_all();
}

extern "C" FIXTURE_EXPORT void async_slow_resume_state(bool* did_return, bool* cancelled, unsigned* destroyed) {
    *did_return = returned.load();
    *cancelled = saw_cancel.load();
    *destroyed = destroys.load();
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** out) {
    if (out == nullptr) {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = &DESCRIPTOR;
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_async_create(const yaddnsc_async_services* services,
                                                                     yaddnsc_async_driver** out, yaddnsc_error* error) {
    if (out == nullptr) {
        write_error(error, YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (services == nullptr || services->struct_size < YADDNSC_ASYNC_SERVICES_MIN_SIZE ||
        services->http_start == nullptr || services->is_cancelled == nullptr) {
        write_error(error, YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    auto* state = new (std::nothrow) State{services};
    if (state == nullptr) {
        write_error(error, YADDNSC_STATUS_INTERNAL_ERROR);
        return YADDNSC_STATUS_INTERNAL_ERROR;
    }
    *out = reinterpret_cast<yaddnsc_async_driver*>(state);  // NOLINT
    return YADDNSC_STATUS_OK;
}

extern "C" FIXTURE_EXPORT void yaddnsc_driver_async_destroy(yaddnsc_async_driver* driver) {
    if (driver != nullptr) {
        delete reinterpret_cast<State*>(driver);  // NOLINT
        destroys.fetch_add(1);
    }
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_begin(yaddnsc_async_driver* driver,
                                                                           const yaddnsc_update_request*,
                                                                           yaddnsc_error* error) {
    auto& state = *reinterpret_cast<State*>(driver);  // NOLINT
    yaddnsc_http_request request{};
    request.struct_size = sizeof(request);
    request.method = YADDNSC_HTTP_GET;
    request.url = {"http://127.0.0.1:1/first", sizeof("http://127.0.0.1:1/first") - 1};
    auto status = state.services->http_start(state.services->context, &request, &state.first, error);
    if (status != YADDNSC_STATUS_OK) {
        return status;
    }
    request.url = {"http://127.0.0.1:2/second", sizeof("http://127.0.0.1:2/second") - 1};
    status = state.services->http_start(state.services->context, &request, &state.second, error);
    return status == YADDNSC_STATUS_OK ? YADDNSC_STATUS_PENDING : status;
}

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_update_async_resume(yaddnsc_async_driver* driver,
                                                                            const yaddnsc_event* event,
                                                                            yaddnsc_error* error) {
    auto& state = *reinterpret_cast<State*>(driver);  // NOLINT
    if (event->kind == YADDNSC_EVENT_UPDATE_CANCELLED) {
        write_error(error, YADDNSC_STATUS_CANCELLED);
        return YADDNSC_STATUS_CANCELLED;
    }
    if (event->op_id != state.first || event->status != YADDNSC_STATUS_OK || notify_entered == nullptr) {
        write_error(error, YADDNSC_STATUS_INVALID_ARGUMENT);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    notify_entered(notification_context);
    released.wait(false);
    // Exercise the borrowed services after the caller has left: they must stay
    // alive through the real worker return, not merely through cancellation.
    saw_cancel.store(state.services->is_cancelled(state.services->context) != 0);
    returned.store(true);
    return YADDNSC_STATUS_PENDING;
}
