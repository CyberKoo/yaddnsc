#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H

#include <memory>

#include "coro/task.hpp"

namespace coro {
class Nursery;
}

namespace plugin::detail {
struct AsyncResources;
struct AsyncEventPacket;

/// Loop-only HTTP event pump. Commands have been copied on the worker and
/// handed back by a joined step before admission. Retiring tasks keep shared
/// resources alive; publication and cancellation never invoke plugin entries.
void apply_async_commands(std::shared_ptr<AsyncResources> resource, coro::Nursery& http_group, bool terminal);
void cancel_all_async_ops(AsyncResources& res);

/// Independent watcher remains runnable while the protocol joins an ABI step.
[[nodiscard]] coro::Task<void> watch_async_cancellation(std::shared_ptr<AsyncResources> resource);
[[nodiscard]] std::shared_ptr<AsyncEventPacket> make_cancel_event();

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H
