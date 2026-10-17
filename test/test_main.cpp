// Shared entry point for every test binary, replacing gtest_main.
//
// Behaves exactly like gtest_main, except the process exits 77 (the automake
// skip convention) when cases were skipped and nothing failed. add_unit_test
// pairs this with SKIP_RETURN_CODE=77 so CTest reports such a binary as
// Skipped instead of Passed: a skip means the environment lacked something
// the test needed, and that must stay visible in CI output instead of being
// silently counted as a pass. Failures keep gtest's exit code, so a binary
// that both fails and skips still reports Failed.

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    const int failures = RUN_ALL_TESTS();
    if (failures == 0 && testing::UnitTest::GetInstance()->skipped_test_count() > 0) {
        return 77;
    }
    return failures;
}
