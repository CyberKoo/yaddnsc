//
// Unit tests for util/fd.hpp — Utils::UniqueFd RAII wrapper.
//
// Verifies:
//   - Default-constructed UniqueFd is invalid.
//   - Move construction transfers ownership; source becomes invalid.
//   - Move assignment transfers ownership; old fd is closed.
//   - reset() replaces the managed fd.
//   - release() transfers ownership to caller; wrapper becomes invalid.
//   - make_pipe() produces valid read/write ends.
//   - make_pipe() fds have close-on-exec set.
//   - Destruction closes the fd.
//   - Copy operations are deleted (compile-time).
// =============================================================================

#include <sys/stat.h>

#include <gtest/gtest.h>

#include "util/fd.hpp"

// ── Default construction ─────────────────────────────────────────────────────

TEST(UniqueFdTest, DefaultConstructor_IsInvalid) {
    Utils::UniqueFd fd;
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

// ── Construction from raw fd ──────────────────────────────────────────────────

TEST(UniqueFdTest, ConstructFromRawFd) {
    // Use /dev/null as a simple fd source.
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd(raw);
    EXPECT_TRUE(fd);
    EXPECT_EQ(fd.get(), raw);
}

// ── Move construction ─────────────────────────────────────────────────────────

TEST(UniqueFdTest, MoveConstructor_TransfersOwnership) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd1(raw);
    Utils::UniqueFd fd2(std::move(fd1));

    EXPECT_TRUE(fd2);
    EXPECT_EQ(fd2.get(), raw);
    EXPECT_FALSE(fd1);
    EXPECT_EQ(fd1.get(), -1);
}

// ── Move assignment ───────────────────────────────────────────────────────────

TEST(UniqueFdTest, MoveAssignment_TransfersOwnership) {
    int raw1 = ::open("/dev/null", O_RDONLY);
    int raw2 = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw1, 0);
    ASSERT_GE(raw2, 0);

    Utils::UniqueFd fd1(raw1);
    Utils::UniqueFd fd2(raw2);

    fd2 = std::move(fd1);

    // fd2 now owns raw1; the old fd2 (raw2) should have been closed.
    EXPECT_TRUE(fd2);
    EXPECT_EQ(fd2.get(), raw1);
    EXPECT_FALSE(fd1);
}

// ── Self-move-assignment ──────────────────────────────────────────────────────

TEST(UniqueFdTest, SelfMoveAssignment_NoOp) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd(raw);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    fd = std::move(fd);

    EXPECT_TRUE(fd);
    EXPECT_EQ(fd.get(), raw);
}

// ── reset() ───────────────────────────────────────────────────────────────────

TEST(UniqueFdTest, Reset_ReplacesFd) {
    int raw1 = ::open("/dev/null", O_RDONLY);
    int raw2 = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw1, 0);
    ASSERT_GE(raw2, 0);

    Utils::UniqueFd fd(raw1);
    fd.reset(raw2);

    EXPECT_TRUE(fd);
    EXPECT_EQ(fd.get(), raw2);
    // raw1 should have been closed by reset()
}

TEST(UniqueFdTest, Reset_ToNegative_BecomesInvalid) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd(raw);
    fd.reset(-1);

    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFdTest, Reset_DefaultParam_BecomesInvalid) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd(raw);
    fd.reset();

    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

// ── release() ─────────────────────────────────────────────────────────────────

TEST(UniqueFdTest, Release_TransfersOwnership) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);

    Utils::UniqueFd fd(raw);
    int released = fd.release();

    EXPECT_EQ(released, raw);
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);

    // Caller is responsible for closing the released fd.
    ::close(released);
}

// ── Destruction closes fd ─────────────────────────────────────────────────────

TEST(UniqueFdTest, Destructor_ClosesFd) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    {
        Utils::UniqueFd read_end(fds[0]);

        // Close write end manually so read end sees EOF on close.
        ::close(fds[1]);

        // read_end goes out of scope here — it should close fds[0].
    }

    // Verify fds[0] is now closed by attempting to read from it.
    char buf;
    // NOLINTNEXTLINE(android-cloexec-fcntl)
    errno = 0;
    auto result = ::read(fds[0], &buf, 1);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, EBADF);
}

// ── make_pipe() ───────────────────────────────────────────────────────────────

TEST(UniqueFdTest, MakePipe_BothEndsValid) {
    auto [read_end, write_end] = Utils::make_pipe();

    EXPECT_TRUE(read_end);
    EXPECT_TRUE(write_end);
    EXPECT_GE(read_end.get(), 0);
    EXPECT_GE(write_end.get(), 0);
}

TEST(UniqueFdTest, MakePipe_WriteAndRead) {
    auto [read_end, write_end] = Utils::make_pipe();

    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);

    const char msg[] = "hello";
    ASSERT_EQ(::write(write_end.get(), msg, sizeof(msg)), static_cast<ssize_t>(sizeof(msg)));

    char buf[16]{};
    ASSERT_EQ(::read(read_end.get(), buf, sizeof(buf)), static_cast<ssize_t>(sizeof(msg)));
    EXPECT_STREQ(buf, "hello");
}

TEST(UniqueFdTest, MakePipe_CloseOnExecSet) {
    auto [read_end, write_end] = Utils::make_pipe();

    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);

    int flags_r = ::fcntl(read_end.get(), F_GETFD);
    int flags_w = ::fcntl(write_end.get(), F_GETFD);
    ASSERT_GE(flags_r, 0);
    ASSERT_GE(flags_w, 0);

    EXPECT_TRUE(flags_r & FD_CLOEXEC);
    EXPECT_TRUE(flags_w & FD_CLOEXEC);
}

// ── UniqueFd is move-only ─────────────────────────────────────────────────────

TEST(UniqueFdTest, IsMoveOnly) {
    EXPECT_TRUE(std::is_move_constructible_v<Utils::UniqueFd>);
    EXPECT_TRUE(std::is_move_assignable_v<Utils::UniqueFd>);
    EXPECT_FALSE(std::is_copy_constructible_v<Utils::UniqueFd>);
    EXPECT_FALSE(std::is_copy_assignable_v<Utils::UniqueFd>);
}

// ── bool conversion ───────────────────────────────────────────────────────────

TEST(UniqueFdTest, BoolConversion_ValidFd_ReturnsTrue) {
    int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);
    Utils::UniqueFd fd(raw);
    EXPECT_TRUE(static_cast<bool>(fd));
}

TEST(UniqueFdTest, BoolConversion_InvalidFd_ReturnsFalse) {
    const Utils::UniqueFd fd;
    EXPECT_FALSE(static_cast<bool>(fd));
}
