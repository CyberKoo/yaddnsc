#ifndef YADDNSC_TEST_COMPONENT_PROCESS_TEST_SUPPORT_H
#define YADDNSC_TEST_COMPONENT_PROCESS_TEST_SUPPORT_H

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ComponentTest {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            old_value_ = previous;
            old_present_ = true;
        }
        if (::setenv(name, value, 1) != 0) {
            throw std::runtime_error("setenv failed");
        }
    }

    ~ScopedEnvVar() noexcept {
        if (old_present_) {
            ::setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::string old_value_;
    bool old_present_ = false;
};

class TempDirectory {
public:
    explicit TempDirectory(std::string pattern) {
        const char* dir = ::mkdtemp(pattern.data());
        if (dir == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = dir;
    }

    ~TempDirectory() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Only the selected case runs in the fresh image. In particular, inherited
// GTEST_REPEAT must not warm its static cache a second time inside that child.
// Used by the single-threaded CA test binaries, before any worker is created.
[[nodiscard]] inline bool run_in_cold_process(const char* binary, const char* test_name) {
    constexpr const char* MARKER = "YADDNSC_CA_COLD_CHILD";
    if (const char* child = std::getenv(MARKER); child != nullptr && std::string_view(child) == test_name) {
        return false;
    }
    const std::string filter = std::string("--gtest_filter=") + test_name;
    const ScopedEnvVar marker(MARKER, test_name);
    const pid_t pid = ::fork();
    if (pid == -1) {
        ADD_FAILURE() << "fork failed";
        return true;
    }
    if (pid == 0) {
        ::execl(binary, binary, filter.c_str(), "--gtest_repeat=1", nullptr);
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    EXPECT_EQ(waited, pid);
    if (waited == pid) {
        EXPECT_TRUE(WIFEXITED(status)) << "cold child terminated abnormally";
        if (WIFEXITED(status)) {
            EXPECT_EQ(WEXITSTATUS(status), 0) << "cold-process test failed";
        }
    }
    return true;
}

}  // namespace ComponentTest

#endif
