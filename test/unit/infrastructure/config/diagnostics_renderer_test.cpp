//
// Unit tests for config/diagnostics/renderer.cpp — render() over
// hand-built Diagnosis values. No file is read: the renderer only formats
// what a decision already settled.
//
// Verified:
//   - Reason::CODE reaches explain_code(), and every ErrorKind the schema
//     cannot explain contributes its own exact sentence to the message.
//   - prose_kind() names JSON null, and a kind with no prose falls back to
//     "an unexpected character" in both the expected and the actual slot.
//
// render_reason()'s trailing `return {}` is deliberately not exercised: the
// switch above it is exhaustive over Reason, so no Diagnosis can reach it.
// =============================================================================

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/config/diagnostics/decision.h"
#include "infrastructure/config/diagnostics/renderer.h"
#include "infrastructure/config/diagnostics/types.h"

namespace {

using Config::Diagnostic::Diagnosis;
using Config::Diagnostic::ErrorKind;
using Config::Diagnostic::Framing;
using Config::Diagnostic::JsonKind;
using Config::Diagnostic::Path;
using Config::Diagnostic::PathComponent;
using Config::Diagnostic::Reason;
using Config::Diagnostic::render;

constexpr std::string_view kConfigPath = "/etc/yaddnsc/config.json";

/// The sentence render() produces when the reason carries no position.
[[nodiscard]] std::string message(std::string_view reason) {
    return "config file \"" + std::string{kConfigPath} + "\": " + std::string{reason};
}

/// A key-only path, so the message shows the dotted form the reader sees.
[[nodiscard]] Path servers_path() {
    return {PathComponent{.key = "resolver"}, PathComponent{.key = "servers"}};
}

/// An ErrorKind together with the wording explain_code() gives it.
struct CodeWording {
    ErrorKind kind;
    std::string_view wording;
};

/// Every code the switch names itself, paired with that exact sentence.
constexpr CodeWording kNamedCodeWordings[] = {
    {ErrorKind::EXPECTED_BRACE, "expected '{' to open an object"},
    {ErrorKind::EXPECTED_BRACKET, "expected '[' to open an array"},
    {ErrorKind::EXPECTED_QUOTE, "expected a quoted string"},
    {ErrorKind::EXPECTED_COLON, "expected ':' after the key"},
    {ErrorKind::UNEXPECTED_END, "the file ends in the middle of a value"},
    {ErrorKind::SYNTAX, "the text is not valid JSON"},
    {ErrorKind::NUMBER, "malformed number"},
    {ErrorKind::INVALID_ESCAPE, "invalid escape sequence in a string"},
    {ErrorKind::INCOMPLETE_UNICODE, "incomplete \\u escape sequence"},
    {ErrorKind::INVALID_UNICODE, "invalid \\u escape sequence"},
    {ErrorKind::DEPTH, "the JSON is nested too deeply"},
    {ErrorKind::EMPTY, "the file is empty"},
    {ErrorKind::MISSING_KEY, "a required key is missing"},
    {ErrorKind::INVALID_NULL, "null is not accepted here"},
    {ErrorKind::CONSTRAINT, "a value constraint was violated"},
    {ErrorKind::UNKNOWN_KEY, "unknown key"},
    {ErrorKind::ENUM, "value is not one of the accepted constants"},
};

/// A kind pair for EXPECTED_TYPE, with the sentence both slots produce.
struct ProseCase {
    JsonKind expected;
    JsonKind actual;
    std::string_view reason;
};

constexpr ProseCase kProseCases[] = {
    {JsonKind::NULL_VALUE, JsonKind::UNEXPECTED, "\"resolver.servers\" expects null, got an unexpected character"},
    {JsonKind::UNEXPECTED, JsonKind::STRING, "\"resolver.servers\" expects an unexpected character, got a string"},
    {JsonKind::STRING, JsonKind::NONE, "\"resolver.servers\" expects a string, got an unexpected character"},
};

}  // namespace

TEST(DiagnosticRendererTest, CodeReason_EveryNamedErrorKind_RendersItsOwnSentence) {
    for (const auto& entry : kNamedCodeWordings) {
        SCOPED_TRACE(entry.wording);
        const Diagnosis diagnosis{
            .site = {},
            .failure = {.kind = entry.kind, .identifier = {}},
            .reason = Reason::CODE,
            .framing = Framing::NONE,
            .show_position = false,
            .append_location = false,
            .show_actual_kind = false,
            .expected = {.type = JsonKind::NONE, .values = {}, .nullable = false},
            .candidates = {},
            .suggestion = {},
        };
        EXPECT_EQ(render(kConfigPath, diagnosis), message(entry.wording));
    }
}

TEST(DiagnosticRendererTest, ExpectedType_KindWithProseOrTheFallback_NamesBothSlots) {
    const Path path = servers_path();
    for (const auto& entry : kProseCases) {
        SCOPED_TRACE(entry.reason);
        const Diagnosis diagnosis{
            .site = {.line = 4, .column = 12, .parent_path = {}, .key = {}, .kind = entry.actual, .path = path},
            .failure = {.kind = ErrorKind::OTHER, .identifier = {}},
            .reason = Reason::EXPECTED_TYPE,
            .framing = Framing::NONE,
            .show_position = false,
            .append_location = false,
            .show_actual_kind = false,
            .expected = {.type = entry.expected, .values = {}, .nullable = false},
            .candidates = {},
            .suggestion = {},
        };
        EXPECT_EQ(render(kConfigPath, diagnosis), message(entry.reason));
    }
}
