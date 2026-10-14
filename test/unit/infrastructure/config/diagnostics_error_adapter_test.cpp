//
// Unit tests for config/diagnostics/error_adapter.cpp — the Glaze
// error_code → internal ParseFailure mapping and the scan policy it picks.
// No JSON is parsed: every case feeds a code straight into the adapter.
//
// Verified:
//   - The lexical codes (syntax, escapes, \u escapes, nesting) and the
//     schema codes (rejected null, violated constraint) each map to their
//     own ErrorKind and carry no identifier.
//   - An unmapped code falls back to ErrorKind::OTHER and keeps the Glaze
//     code name, which is what explain_code() later reads back as prose.
//   - scan_policy() turns on key-offset scanning only for an unknown key
//     and complete-string inspection only for an enum mismatch.
// =============================================================================

#include <string>

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include "infrastructure/config/diagnostics/error_adapter.h"
#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/types.h"

namespace {

using Config::Diagnostic::adapt_error;
using Config::Diagnostic::ErrorKind;
using Config::Diagnostic::ParseFailure;
using Config::Diagnostic::scan_policy;

/// A Glaze code and the ErrorKind the adapter maps it to.
struct CodeMapping {
    glz::error_code code;
    ErrorKind kind;
};

/// The codes the switch names one by one instead of falling back.
constexpr CodeMapping kMappedCodes[] = {
    {glz::error_code::syntax_error, ErrorKind::SYNTAX},
    {glz::error_code::invalid_escape, ErrorKind::INVALID_ESCAPE},
    {glz::error_code::u_requires_hex_digits, ErrorKind::INCOMPLETE_UNICODE},
    {glz::error_code::unicode_escape_conversion_failure, ErrorKind::INVALID_UNICODE},
    {glz::error_code::exceeded_max_recursive_depth, ErrorKind::DEPTH},
    {glz::error_code::invalid_nullable_read, ErrorKind::INVALID_NULL},
    {glz::error_code::constraint_violated, ErrorKind::CONSTRAINT},
};

/// The scan policy an ErrorKind selects.
struct ScanFlags {
    ErrorKind kind;
    bool key_offset_after_quote;
    bool inspect_complete_string;
};

constexpr ScanFlags kScanFlags[] = {
    {.kind = ErrorKind::UNKNOWN_KEY, .key_offset_after_quote = true, .inspect_complete_string = false},
    {.kind = ErrorKind::ENUM, .key_offset_after_quote = false, .inspect_complete_string = true},
    {.kind = ErrorKind::OTHER, .key_offset_after_quote = false, .inspect_complete_string = false},
    {.kind = ErrorKind::SYNTAX, .key_offset_after_quote = false, .inspect_complete_string = false},
    {.kind = ErrorKind::EMPTY, .key_offset_after_quote = false, .inspect_complete_string = false},
};

}  // namespace

TEST(DiagnosticErrorAdapterTest, AdaptError_LexicalAndSchemaCodes_MapToTheirOwnKind) {
    for (const auto& entry : kMappedCodes) {
        SCOPED_TRACE(static_cast<int>(entry.kind));
        const ParseFailure failure = adapt_error(entry.code);
        EXPECT_EQ(failure.kind, entry.kind);
        EXPECT_TRUE(failure.identifier.empty());
    }
}

TEST(DiagnosticErrorAdapterTest, AdaptError_UnmappedCode_FallsBackToOtherKeepingTheGlazeName) {
    const ParseFailure failure = adapt_error(glz::error_code::invalid_flag_input);
    EXPECT_EQ(failure.kind, ErrorKind::OTHER);
    EXPECT_EQ(failure.identifier, "invalid_flag_input");
}

TEST(DiagnosticErrorAdapterTest, ScanPolicy_OnlyUnknownKeyAndEnumRaiseTheirFlag) {
    for (const auto& entry : kScanFlags) {
        SCOPED_TRACE(static_cast<int>(entry.kind));
        const auto policy = scan_policy(ParseFailure{.kind = entry.kind, .identifier = {}});
        EXPECT_EQ(policy.key_offset_after_quote, entry.key_offset_after_quote);
        EXPECT_EQ(policy.inspect_complete_string, entry.inspect_complete_string);
    }
}
