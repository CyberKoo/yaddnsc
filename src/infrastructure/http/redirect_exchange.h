#ifndef YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_EXCHANGE_H
#define YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_EXCHANGE_H

#include <expected>
#include <string>

#include "coro/task.hpp"
#include "infrastructure/http/error.h"
#include "infrastructure/http/types.h"

namespace http {
class Session;

namespace detail {
/// One logical exchange owns the redirect budget, even when it leaves a
/// persistent origin. Borrows options/session through completion. Cancellation
/// propagates; HTTP failures are values. Loop-thread only.
[[nodiscard]] coro::Task<std::expected<Response, Error>> exchange_redirects(std::string url, Request request,
                                                                            const Options& options,
                                                                            Session* initial_session = nullptr);
}  // namespace detail
}  // namespace http

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_REDIRECT_EXCHANGE_H
