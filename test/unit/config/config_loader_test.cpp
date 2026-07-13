//
// Unit tests for src/config/config.cpp — configuration loading.
//
// Verifies:
//   - Existing valid config file → parses successfully.
//   - Non-existent file → throws std::runtime_error.
//   - Invalid JSON content → throws std::runtime_error.
//   - Missing required fields → throws std::runtime_error.
// =============================================================================

#include <cstdio>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "config/config.h"

// ── Helper: write a temp config file ───────────────────────────────────────

[[nodiscard]] std::string write_temp_config(std::string_view content) {
    auto path = std::filesystem::temp_directory_path() / "yaddnsc_test_config_XXXXXX.json";

    // Generate a unique filename.
    char template_path[] = "/tmp/yaddnsc_test_config_XXXXXX.json";
    int fd = ::mkstemps(template_path, 5);  // 5 = length of ".json"
    if (fd < 0) {
        throw std::runtime_error("Failed to create temp file");
    }
    ::write(fd, content.data(), content.size());
    ::close(fd);
    return template_path;
}

[[nodiscard]] std::string get_minimal_valid_config() {
    return R"({
        "driver": {
            "auto_discover": false,
            "load": []
        },
        "resolver": {
            "use_custom_server": false,
            "strategy": "fallback"
        },
        "domains": []
    })";
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, LoadValidConfig_Succeeds) {
    auto path = write_temp_config(get_minimal_valid_config());

    EXPECT_NO_THROW({
        auto cfg = Config::load_config(path);
    });

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, NonExistentFile_ThrowsRuntimeError) {
    EXPECT_THROW(
        { [[maybe_unused]] auto cfg = Config::load_config("/tmp/nonexistent_config_12345.json"); },
        std::runtime_error
    );
}

TEST(ConfigLoaderTest, InvalidJson_ThrowsRuntimeError) {
    auto path = write_temp_config("{invalid json content!!!}");

    EXPECT_THROW(
        { [[maybe_unused]] auto cfg = Config::load_config(path); },
        std::runtime_error
    );

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, EmptyFile_ThrowsRuntimeError) {
    auto path = write_temp_config("");

    EXPECT_THROW(
        { [[maybe_unused]] auto cfg = Config::load_config(path); },
        std::runtime_error
    );

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, MissingRequiredField_ThrowsRuntimeError) {
    // Missing "resolver" section.
    auto path = write_temp_config(R"({"driver": {"directory": "/x", "load": []}})");

    EXPECT_THROW(
        { [[maybe_unused]] auto cfg = Config::load_config(path); },
        std::runtime_error
    );

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, ConfigFields_ArePopulated) {
    auto path = write_temp_config(get_minimal_valid_config());

    auto cfg = Config::load_config(path);
    EXPECT_FALSE(cfg.resolver.use_custom_server);
    EXPECT_TRUE(cfg.domains.empty());
    EXPECT_TRUE(cfg.driver.load.empty());

    std::filesystem::remove(path);
}
