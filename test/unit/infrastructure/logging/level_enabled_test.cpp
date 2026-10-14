//
// Unit tests for logging::level_enabled (src/infrastructure/logging/level_enabled.h).
//
// The helper guards log arguments that are expensive to build or have side
// effects, so it must answer exactly what the default logger would do: follow
// the active spdlog level, and report "disabled" instead of dereferencing a
// null logger once logging has shut down.
// =============================================================================

#include "infrastructure/logging/level_enabled.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace {

/// The current default logger as an owning copy, or null when none is
/// installed.
///
/// Not `shared_ptr(raw)`: that mints a second control block instead of sharing
/// the registry's, so the two refcounts drift and whichever drops last deletes
/// a logger the other still points at. spdlog::default_logger() throws on a
/// null default, hence the guard.
[[nodiscard]] std::shared_ptr<spdlog::logger> current_default_logger() {
    if (spdlog::default_logger_raw() == nullptr) {
        return nullptr;
    }
    return spdlog::default_logger();
}

/// Install a silent logger at `level` as the spdlog default for one case and
/// put the previous default back on scope exit (nullptr restores the "no
/// default logger" state).
class ScopedDefaultLogger {
public:
    explicit ScopedDefaultLogger(spdlog::level::level_enum level) : original_(current_default_logger()) {
        logger_ = std::make_shared<spdlog::logger>("yaddnsc_level_enabled_test",
                                                   std::make_shared<spdlog::sinks::null_sink_mt>());
        logger_->set_level(level);
        spdlog::set_default_logger(logger_);
    }

    ~ScopedDefaultLogger() { spdlog::set_default_logger(original_); }

    ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
    ScopedDefaultLogger& operator=(const ScopedDefaultLogger&) = delete;

private:
    std::shared_ptr<spdlog::logger> original_;
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace

TEST(LevelEnabled, FollowsTheActiveSpdlogLevel) {
    const ScopedDefaultLogger env(spdlog::level::info);

    // The guard decides whether an argument is built, so a level below the
    // active one must report disabled for every severity under it.
    EXPECT_FALSE(logging::level_enabled(spdlog::level::trace));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::debug));
    EXPECT_TRUE(logging::level_enabled(spdlog::level::info));
    EXPECT_TRUE(logging::level_enabled(spdlog::level::warn));
    EXPECT_TRUE(logging::level_enabled(spdlog::level::err));
    EXPECT_TRUE(logging::level_enabled(spdlog::level::critical));
}

TEST(LevelEnabled, RaisesTheThresholdWhenTheLevelChanges) {
    const ScopedDefaultLogger env(spdlog::level::trace);
    EXPECT_TRUE(logging::level_enabled(spdlog::level::trace));

    // The active level is read per call, not cached: a guard around a slow
    // record has to follow a level changed at runtime.
    spdlog::default_logger()->set_level(spdlog::level::err);
    EXPECT_FALSE(logging::level_enabled(spdlog::level::warn));
    EXPECT_TRUE(logging::level_enabled(spdlog::level::err));
}

TEST(LevelEnabled, AfterLoggingShutdown_ReturnsFalseWithoutDefaultLogger) {
    const ScopedDefaultLogger env(spdlog::level::trace);
    ASSERT_NE(spdlog::default_logger_raw(), nullptr);

    spdlog::shutdown();  // drops every registered logger, the pipeline is gone

    // Nothing can be written any more, so every guarded argument stays
    // unevaluated instead of dereferencing a null logger.
    EXPECT_EQ(spdlog::default_logger_raw(), nullptr);
    EXPECT_FALSE(logging::level_enabled(spdlog::level::trace));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::debug));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::info));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::warn));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::err));
    EXPECT_FALSE(logging::level_enabled(spdlog::level::critical));
}
