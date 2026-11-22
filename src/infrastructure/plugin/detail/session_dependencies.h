#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_SESSION_DEPENDENCIES_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_SESSION_DEPENDENCIES_H

#include <chrono>

#include "infrastructure/http/types.h"

namespace app {
class LoggerPort;
}

namespace coro {
class Loop;
class Nursery;
}  // namespace coro

namespace plugin {

/// Concrete loop-side capabilities used to create one driver session. The
/// logger, loop and nursery are borrowed and must outlive the session drain.
struct SessionDependencies {
    const app::LoggerPort& logger;
    coro::Loop& loop;
    coro::Nursery& cycle_nursery;
    http::Options http;
    std::chrono::milliseconds exchange_budget;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_SESSION_DEPENDENCIES_H
