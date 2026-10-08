//
// Unit tests for the asynchronous logging pipeline
// (src/infrastructure/logging/async_logging.cpp).
//
// The test installs its own recording sink as the default logger, brings up the
// async pipeline over it, emits through both the Logger facade and a direct
// SPDLOG_* macro, checks the overrun counter is readable, drains the pipeline,
// and only then asserts — the sink is written by the drain thread, so reading it
// before shutdown would race.
// =============================================================================

#include "infrastructure/logging/async_logging.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/async.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "application/ports/log.h"
#include "infrastructure/logging/spdlog_logger.h"

namespace {

/// spdlog sink that records every payload it receives.
class CollectingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        messages.emplace_back(msg.payload.data(), msg.payload.size());
    }

    void flush_() override {}
};

[[nodiscard]] bool contains(const std::shared_ptr<CollectingSink>& sink, const std::string& needle) {
    return std::find(sink->messages.begin(), sink->messages.end(), needle) != sink->messages.end();
}

}  // namespace

TEST(AsyncLogging, FacadeAndSpdlogMacroShareTheAsyncPipeline) {
    // A synchronous default logger over our sink is the starting point; the
    // pipeline adopts its sinks, level and pattern.
    auto sink = std::make_shared<CollectingSink>();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("", sink));

    logging::initialize();

    // The overrun counter is readable and starts clean. The handle is scoped so
    // it does not outlive shutdown(): keeping a pool reference would stop
    // spdlog::shutdown() from destroying (and therefore draining) it.
    {
        const auto pool = spdlog::thread_pool();
        ASSERT_NE(pool, nullptr);
        EXPECT_EQ(pool->overrun_counter(), 0u);
    }

    SPDLOG_INFO("direct-spdlog-message");
    const SpdlogLogger facade;
    facade.log(LogLevel::INFO, "facade-message", std::source_location::current());

    // Drains the queue and joins the drain thread, so the sink is stable.
    logging::shutdown();

    EXPECT_TRUE(contains(sink, "direct-spdlog-message"));
    EXPECT_TRUE(contains(sink, "facade-message"));

    // Restore a synchronous default logger: the pipeline is gone, and a later
    // case in this binary must not log into a torn-down pool.
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("", sink));
}
