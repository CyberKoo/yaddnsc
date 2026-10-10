//
// Shared helpers for CLI tests: argv construction, temp config files, and
// stdout/stderr capture. Used by both the pure unit tests (test/unit/cli/)
// and the dispatch component tests (test/component/cli_test.cpp).
// =============================================================================

#ifndef YADDNSC_TEST_FIXTURES_CLI_TEST_SUPPORT_H
#define YADDNSC_TEST_FIXTURES_CLI_TEST_SUPPORT_H

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "cli/parser.h"

namespace CliTestSupport {

struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    [[nodiscard]] int argc() const { return static_cast<int>(ptrs.size()); }

    [[nodiscard]] char** data() { return ptrs.data(); }
};

/// Build argv that stays alive for the duration of the call: the returned
/// struct owns both the string storage and the char* pointers.
[[nodiscard]] inline Argv make_argv(std::vector<std::string> args) {
    Argv argv;
    argv.storage = std::move(args);
    argv.ptrs.reserve(argv.storage.size());
    for (auto& arg : argv.storage) {
        argv.ptrs.push_back(arg.data());
    }
    return argv;
}

/// Parse helper: builds argv and runs the pure parser.
[[nodiscard]] inline Cli::ParseResult parse(std::vector<std::string> args) {
    auto argv = make_argv(std::move(args));
    return Cli::parse(argv.argc(), argv.data());
}

class TempConfigFile {
public:
    explicit TempConfigFile(std::string content) : path_(make_unique_path()) {
        std::ofstream out(path_);
        out << content;
    }

    ~TempConfigFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempConfigFile(const TempConfigFile&) = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    [[nodiscard]] static std::string make_unique_path() {
        static std::atomic<unsigned> counter{0};
        const auto path =
            std::filesystem::temp_directory_path() /
            ("yaddnsc_cli_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) + ".json");
        return path.string();
    }

    std::string path_;
};

/// Redirect a stream (STDOUT_FILENO or STDERR_FILENO) to a temp file so
/// output can be asserted. Restores on destruction (or when str() runs).
class StreamCapture {
public:
    explicit StreamCapture(int fd = STDOUT_FILENO) : target_fd_(fd), path_(make_unique_path()) {
        flush_out();
        saved_fd_ = ::dup(target_fd_);
        file_fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(file_fd_, target_fd_);
    }

    ~StreamCapture() {
        restore();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    /// Restore the stream and return everything written so far.
    [[nodiscard]] std::string str() {
        restore();
        std::ifstream in(path_);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    // std::println writes to the stdio buffer while CLI11 and gtest use
    // std::cout — both buffers must be drained around a redirect.
    static void flush_out() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
    }

    void restore() {
        if (saved_fd_ == -1) {
            return;
        }
        flush_out();
        ::dup2(saved_fd_, target_fd_);
        ::close(saved_fd_);
        ::close(file_fd_);
        saved_fd_ = -1;
    }

    [[nodiscard]] static std::filesystem::path make_unique_path() {
        static std::atomic<unsigned> counter{0};
        return std::filesystem::temp_directory_path() /
               ("yaddnsc_cli_out_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) + ".txt");
    }

    int target_fd_;
    std::filesystem::path path_;
    int saved_fd_{-1};
    int file_fd_{-1};
};

using StdoutCapture = StreamCapture;

}  // namespace CliTestSupport

#endif  // YADDNSC_TEST_FIXTURES_CLI_TEST_SUPPORT_H
