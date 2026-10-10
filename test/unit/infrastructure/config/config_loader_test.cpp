//
// Unit tests for src/infrastructure/config/config.cpp — configuration loading.
//
// Verifies:
//   - Existing valid config file → parses successfully.
//   - Non-existent file → throws std::runtime_error.
//   - Invalid JSON content → throws std::runtime_error.
//   - Missing required fields → throws std::runtime_error.
//   - A parse failure names the position, the key, and what that key accepts,
//     without ever echoing a configuration value.
// =============================================================================

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include "infrastructure/config/config.h"
#include "infrastructure/config/diagnostics/decision.h"
#include "infrastructure/config/diagnostics/error_adapter.h"
#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/parse_diagnostic.h"
#include "infrastructure/config/diagnostics/renderer.h"
#include "infrastructure/config/diagnostics/schema.h"
#include "infrastructure/config/diagnostics/types.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations

// ── Helper: write a temp config file ───────────────────────────────────────

[[nodiscard]] std::string write_temp_config(std::string_view content) {
    auto path = std::filesystem::temp_directory_path() / "yaddnsc_test_config_XXXXXX.json";

    // Generate a unique filename.
    char template_path[] = "/tmp/yaddnsc_test_config_XXXXXX.json";
    int fd = ::mkstemps(template_path, 5);  // 5 = length of ".json"
    if (fd < 0) {
        throw std::runtime_error("Failed to create temp file");
    }
    const auto written = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (written < 0 || static_cast<size_t>(written) != content.size()) {
        throw std::runtime_error("Failed to write temp config file");
    }
    return template_path;
}

[[nodiscard]] std::string get_minimal_valid_config() {
    return R"({
        "drivers": {
            "auto_discover": false,
            "load": []
        },
        "resolver": {
            "use_custom_servers": false,
            "strategy": "fallback"
        },
        "domains": []
    })";
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, LoadValidConfig_Succeeds) {
    auto path = write_temp_config(get_minimal_valid_config());

    EXPECT_NO_THROW({ auto cfg = Config::load_config(path); });

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, NonExistentFile_ThrowsRuntimeError) {
    EXPECT_THROW(
        { [[maybe_unused]] auto cfg = Config::load_config("/tmp/nonexistent_config_12345.json"); }, std::runtime_error);
}

TEST(ConfigLoaderTest, InvalidJson_ThrowsRuntimeError) {
    auto path = write_temp_config("{invalid json content!!!}");

    EXPECT_THROW({ [[maybe_unused]] auto cfg = Config::load_config(path); }, std::runtime_error);

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, InvalidJson_ErrorDoesNotLeakConfigContent) {
    // Regression: the exception message must not contain the raw config
    // file contents — they may hold API credentials and the message is
    // logged as a fatal error.
    auto path = write_temp_config(R"({"token": "supersecret456", "broken")");

    try {
        [[maybe_unused]] auto cfg = Config::load_config(path);
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        EXPECT_EQ(msg.find("supersecret456"), std::string::npos) << "message leaked config content: " << msg;
    }

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, InvalidJson_ErrorDoesNotLeakCredentialOnTheFailingLine) {
    // The rejected key sits on the same line as a credential. The message
    // must still carry only the key name, never the value next to it.
    auto path = write_temp_config(
        R"({"drivers": {"auto_discover": false, "api_token": "leaked789", "nope": 1}, "resolver": {"use_custom_servers": false, "strategy": "fallback"}, "domains": []})");

    try {
        [[maybe_unused]] auto cfg = Config::load_config(path);
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        EXPECT_EQ(msg.find("leaked789"), std::string::npos) << "message leaked a config value: " << msg;
        EXPECT_NE(msg.find("unknown key \"api_token\""), std::string::npos)
            << "message should name the offending key: " << msg;
        EXPECT_NE(msg.find("keys accepted here"), std::string::npos)
            << "message should list the accepted keys: " << msg;
    }

    std::filesystem::remove(path);
}

// ── Parse diagnostics ──────────────────────────────────────────────────────
//
// A bare glaze error code ("unknown_key") tells the user nothing to act on.
// These cases pin the parts that make a failure actionable.

/// Load @p content and return the message of the expected failure.
[[nodiscard]] std::string load_failure_message(std::string_view content) {
    const auto path = write_temp_config(content);
    try {
        [[maybe_unused]] auto cfg = Config::load_config(path);
        std::filesystem::remove(path);
        return {};
    } catch (const std::runtime_error& e) {
        std::filesystem::remove(path);
        return e.what();
    }
}

TEST(ConfigLoaderTest, UnknownKey_NamesTheKeyAndSuggestsTheClosestOne) {
    const auto message = load_failure_message(R"({
    "drivers": {
        "auto_discover": false,
        "driver_dire": "/opt/drivers"
    },
    "resolver": { "use_custom_servers": false, "strategy": "fallback" },
    "domains": []
})");

    EXPECT_NE(message.find("unknown key \"driver_dire\""), std::string::npos) << message;
    EXPECT_NE(message.find("in \"drivers\""), std::string::npos) << message;
    EXPECT_NE(message.find("did you mean \"driver_dir\""), std::string::npos) << message;
    EXPECT_NE(message.find("line 4"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, UnknownKeyWithoutCloseMatch_ListsTheKeysAcceptedThere) {
    const auto message = load_failure_message(R"({
    "resolver": { "use_custom_servers": false, "strategy": "fallback", "zzzzzzzzzz": 1 },
    "domains": []
})");

    EXPECT_NE(message.find("unknown key \"zzzzzzzzzz\""), std::string::npos) << message;
    EXPECT_NE(message.find("keys accepted here"), std::string::npos) << message;
    EXPECT_NE(message.find("\"strategy\""), std::string::npos) << message;
}

TEST(ConfigLoaderTest, UnexpectedEnum_ListsTheAcceptedValues) {
    const auto message = load_failure_message(R"({
    "resolver": { "use_custom_servers": false, "strategy": "parralell" },
    "domains": []
})");

    EXPECT_NE(message.find("\"resolver.strategy\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"fallback\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"concurrent\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"shuffle\""), std::string::npos) << message;
}

TEST(ConfigLoaderTest, WrongType_NamesTheKeyAndTheExpectedType) {
    const auto message = load_failure_message(R"({
    "drivers": { "auto_discover": "yes" },
    "resolver": { "use_custom_servers": false, "strategy": "fallback" },
    "domains": []
})");

    EXPECT_NE(message.find("\"drivers.auto_discover\""), std::string::npos) << message;
    EXPECT_NE(message.find("boolean"), std::string::npos) << message;
    EXPECT_NE(message.find("got a string"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, WrongTypeInNestedList_ReportsTheFullPath) {
    const auto message = load_failure_message(R"({
    "resolver": { "use_custom_servers": false, "strategy": "fallback" },
    "domains": [{ "name": "a.com", "subdomains": [{ "name": "@", "allow_ula": 1 }] }]
})");

    EXPECT_NE(message.find("\"domains[0].subdomains[0].allow_ula\""), std::string::npos) << message;
    EXPECT_NE(message.find("boolean"), std::string::npos) << message;
    EXPECT_NE(message.find("got a number"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, WrongEnumInNestedList_ListsTheAcceptedValues) {
    const auto message = load_failure_message(R"({
    "resolver": { "use_custom_servers": false, "strategy": "fallback" },
    "domains": [{ "name": "a.com", "subdomains": [{ "name": "@", "type": "aaaaa" }] }]
})");

    EXPECT_NE(message.find("\"domains[0].subdomains[0].type\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"a\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"txt\""), std::string::npos) << message;
}

TEST(ConfigLoaderTest, QuotedNumber_NamesTheKeyAndTheExpectedType) {
    // Glaze stops at the opening quote of the value here, before consuming it.
    const auto message = load_failure_message(R"({
    "resolver": { "use_custom_servers": true, "servers": [{ "address": "1.1.1.1", "port": "53" }] },
    "domains": []
})");

    EXPECT_NE(message.find("\"resolver.servers[0].port\""), std::string::npos) << message;
    EXPECT_NE(message.find("got a string"), std::string::npos) << message;
    // The document is well-formed JSON; only the value is wrong.
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, MissingColon_NamesTheKeyAndTheMissingToken) {
    const auto message = load_failure_message(R"({"drivers" {"auto_discover": false}})");

    EXPECT_NE(message.find("invalid JSON"), std::string::npos) << message;
    EXPECT_NE(message.find("expected ':' after the key \"drivers\""), std::string::npos) << message;
}

TEST(ConfigLoaderTest, TruncatedFile_SaysTheFileEndsThere) {
    const auto message = load_failure_message("{\n    \"drivers\": { \"auto_discover\": false },");

    EXPECT_NE(message.find("the file ends here"), std::string::npos) << message;
    EXPECT_NE(message.find("line 2"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, ObjectExpectedForMember_NamesTheOffendingKey) {
    // Well-formed JSON with the wrong shape: the schema can name the key.
    const auto message = load_failure_message(R"({
    "drivers": [],
    "resolver": { "use_custom_servers": false, "strategy": "fallback" },
    "domains": []
})");

    EXPECT_NE(message.find("line 2"), std::string::npos) << message;
    EXPECT_NE(message.find("\"drivers\" expects an object"), std::string::npos) << message;
    EXPECT_NE(message.find("got an array"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, EmptyFile_SaysTheFileIsEmpty) {
    const auto message = load_failure_message("");

    EXPECT_NE(message.find("the file is empty"), std::string::npos) << message;
    EXPECT_NE(message.find("line 1"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, EveryFailure_KeepsTheBareGlazeCodeOutOfTheMessage) {
    // "unknown_key" and friends are library vocabulary, not a diagnosis.
    for (const auto* content : {R"({"drivers": {"nope": 1}})", R"({)", "", R"({"drivers": 5})"}) {
        const auto message = load_failure_message(content);
        EXPECT_EQ(message.find("unknown_key"), std::string::npos) << message;
        EXPECT_EQ(message.find("expected_true_or_false"), std::string::npos) << message;
        EXPECT_NE(message.find("config file \""), std::string::npos) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_WrongSecondOrThirdDriverElement_ReportsIndexTypeAndExactPosition) {
    struct Case {
        std::string_view value;
        std::string_view actual_type;
    };

    const Case cases[] = {{"2", "a number"}, {"true", "true or false"}, {"{}", "an object"}};
    for (const auto& test_case : cases) {
        for (const std::size_t index : {1U, 2U}) {
            SCOPED_TRACE(std::string(test_case.value) + " at index " + std::to_string(index));
            std::string content = "{\n\"drivers\":{\"load\":[\n  \"one\",\n";
            if (index == 2) {
                content += "  \"two\",\n";
            }
            content += "  ";
            content += test_case.value;
            content += "\n]}}";
            const auto message = load_failure_message(content);
            EXPECT_NE(message.find("\"drivers.load[" + std::to_string(index) + "]\" expects a string"),
                      std::string::npos)
                << message;
            EXPECT_NE(message.find("got " + std::string(test_case.actual_type)), std::string::npos) << message;
            EXPECT_NE(message.find("(line " + std::to_string(index + 3) + ", column 3):"), std::string::npos)
                << message;
            EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
        }
    }
}

TEST(ConfigLoaderTest, LoadConfig_WrongSecondDomainElement_ReportsObjectSchemaAndExactPosition) {
    const auto message = load_failure_message("{\n\"domains\":[\n  {},\n  5\n]}");

    EXPECT_NE(message.find("\"domains[1]\" expects an object"), std::string::npos) << message;
    EXPECT_NE(message.find("got a number"), std::string::npos) << message;
    EXPECT_NE(message.find("(line 4, column 3):"), std::string::npos) << message;
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, LoadConfig_NestedNonzeroIndices_ReportsCompletePathAndExactPosition) {
    const auto message =
        load_failure_message("{\"domains\":[\n{} ,\n{\"subdomains\":[\n{},\n{},\n{\"allow_ula\":1}\n]}\n]}");

    EXPECT_NE(message.find("\"domains[1].subdomains[2].allow_ula\" expects a boolean"), std::string::npos) << message;
    EXPECT_NE(message.find("got a number"), std::string::npos) << message;
    EXPECT_NE(message.find("(line 6, column 14):"), std::string::npos) << message;
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, DescribeParseError_EmptyBufferWithFileOperationError_ReportsIoWithoutContentPosition) {
    struct Case {
        glz::error_code code;
        std::string_view reason;
    };

    const Case cases[] = {
        {glz::error_code::file_open_failure, "the file could not be opened"},
        {glz::error_code::file_close_failure, "the file could not be closed"},
        {glz::error_code::file_include_error, "an included file could not be read"},
        {glz::error_code::file_extension_not_supported, "the file extension is not supported"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.reason);
        const auto message = Config::Diagnostic::describe_parse_error("unreadable.json", "", 0, test_case.code);
        EXPECT_EQ(message, "config file \"unreadable.json\": " + std::string(test_case.reason));
    }
}

TEST(ConfigLoaderTest, DescribeParseError_EmptyInputWithoutIoError_ReportsEmptyFileAtFirstByte) {
    EXPECT_EQ(Config::Diagnostic::describe_parse_error("empty.json", "", 0, glz::error_code::no_read_input),
              "config file \"empty.json\" (line 1, column 1): the file is empty");
}

TEST(ConfigLoaderTest, LoadConfig_MalformedSyntax_ReportsSyntaxRatherThanEnumOrTypeExpectation) {
    struct Case {
        std::string_view content;
        std::string_view reason;
        std::string_view position;
        glz::error_code code;
        std::size_t offset;
    };

    const Case cases[] = {
        {R"({"drivers" {"auto_discover":false}})", "expected ':'",
         "(line 1, column 2):", glz::error_code::expected_colon, 11},
        {R"({"resolver":{"strategy":"fallback" "use_custom_servers":false}})", "expected ','",
         "(line 1, column 25):", glz::error_code::expected_comma, 35},
        {R"({"resolver":{"strategy":"fall\qback"}})", "malformed or unterminated string",
         "(line 1, column 25):", glz::error_code::unexpected_enum, 25},
        {R"({"resolver":{"strategy":"fallback)", "malformed or unterminated string",
         "(line 1, column 25):", glz::error_code::unexpected_enum, 25},
    };
    for (const auto& test_case : cases) {
        Config::AppConfig config;
        const auto error = glz::read_json(config, test_case.content);
        SCOPED_TRACE(std::string(test_case.content));
        SCOPED_TRACE("Glaze code=" + glz::format_error(error.ec) + ", offset=" + std::to_string(error.count));
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, test_case.code);
        EXPECT_EQ(error.count, test_case.offset);
        const auto message = load_failure_message(test_case.content);
        EXPECT_NE(message.find("invalid JSON"), std::string::npos) << message;
        EXPECT_NE(message.find(test_case.reason), std::string::npos) << message;
        EXPECT_NE(message.find(test_case.position), std::string::npos) << message;
        EXPECT_EQ(message.find("expects"), std::string::npos) << message;
        EXPECT_EQ(message.find("one of"), std::string::npos) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_CompletedArrayElementWithoutSeparator_ReportsContainerBoundary) {
    using namespace Config::Diagnostic;

    struct Case {
        std::string_view content;
        std::size_t offset;
        std::string_view path;
    };

    const Case cases[] = {
        {R"({"drivers": {"load": ["a" "b"]}})", 26, "drivers.load"},
        {R"({"drivers": {"load": ["a")", 25, "drivers.load"},
        {R"({"domains":[{} {})", 15, "domains"},
        {R"({"domains":[{})", 14, "domains"},
        {R"({"domains":[{}, {"subdomains":[{}, {} {})", 38, "domains[1].subdomains"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.content);
        Config::AppConfig config;
        const auto error = glz::read_json(config, test_case.content);
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, glz::error_code::expected_bracket);
        EXPECT_EQ(error.count, test_case.offset);
        const auto expected = "(line 1, column " + std::to_string(test_case.offset + 1) +
                              "): invalid JSON — expected ',' or ']' after an array element at \"" +
                              std::string(test_case.path) + '"';
        EXPECT_EQ(describe_parse_error("boundary.json", test_case.content, error.count, error.ec),
                  "config file \"boundary.json\" " + expected);
        const auto message = load_failure_message(test_case.content);
        EXPECT_NE(message.find(expected), std::string::npos) << message;
        EXPECT_EQ(message.find("expects"), std::string::npos) << message;
        EXPECT_EQ(message.find("the file ends here"), std::string::npos) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_CompletedObjectMemberAtEof_PreservesActualCommaFailure) {
    using namespace Config::Diagnostic;
    const std::string_view content = R"({"drivers":{"auto_discover":true)";
    Config::AppConfig config;
    const auto error = glz::read_json(config, content);
    ASSERT_TRUE(error);
    EXPECT_EQ(error.ec, glz::error_code::expected_comma);
    EXPECT_EQ(error.count, 32U);
    const auto site = locate(content, error.count);
    EXPECT_EQ(site.after_value.kind, JsonKind::OBJECT);
    EXPECT_EQ(site.after_value.column, 33U);
    const auto message = load_failure_message(content);
    EXPECT_NE(message.find("invalid JSON — expected ',' or '}' at \"drivers.auto_discover\""), std::string::npos)
        << message;
    EXPECT_EQ(message.find("after an object member"), std::string::npos) << message;
    EXPECT_EQ(message.find("expects"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, DescribeParseError_ObjectBoundaryAtEof_ReportsObjectSeparatorAndContainerPath) {
    using namespace Config::Diagnostic;
    // Inject the adapter code: AppConfig's object reader reports expected_comma at EOF instead.
    const std::string_view content = R"({"drivers":{"auto_discover":true)";
    EXPECT_EQ(describe_parse_error("boundary.json", content, content.size(), glz::error_code::unexpected_end),
              "config file \"boundary.json\" (line 1, column 33): invalid JSON — "
              "expected ',' or '}' after an object member at \"drivers\"");
    const std::string_view root = R"({"drivers":{})";
    EXPECT_EQ(describe_parse_error("boundary.json", root, root.size(), glz::error_code::expected_brace),
              "config file \"boundary.json\" (line 1, column 14): invalid JSON — "
              "expected ',' or '}' after an object member");
}

TEST(ConfigLoaderTest, LoadConfig_SeparatorWhereArrayElementExpected_ReportsMissingValue) {
    // Glaze rejects a leading, double, or trailing comma where the next array
    // element should start. The diagnostic must name the container and the
    // offending token, never an element index that never existed, and must not
    // downgrade unconditional syntax damage to a read failure.
    struct Case {
        std::string_view content;
        glz::error_code code;
        std::size_t offset;
        char found;
        std::string_view path;
    };

    const Case cases[] = {
        {R"({"drivers":{"load":["a",]}})", glz::error_code::expected_quote, 24, ']', "drivers.load"},
        {R"({"drivers":{"load":["a" , ]}})", glz::error_code::expected_quote, 26, ']', "drivers.load"},
        {R"({"drivers":{"load":[,]}})", glz::error_code::expected_quote, 20, ',', "drivers.load"},
        {R"({"drivers":{"load":["a",,]}})", glz::error_code::expected_quote, 24, ',', "drivers.load"},
        {R"({"domains":[{},]})", glz::error_code::expected_brace, 15, ']', "domains"},
        {R"({"domains":[})", glz::error_code::expected_brace, 12, '}', "domains"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.content);
        Config::AppConfig config;
        const auto error = glz::read_json(config, test_case.content);
        SCOPED_TRACE("Glaze code=" + glz::format_error(error.ec) + ", offset=" + std::to_string(error.count));
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, test_case.code);
        EXPECT_EQ(error.count, test_case.offset);
        const auto message = load_failure_message(test_case.content);
        const auto expected = "(line 1, column " + std::to_string(test_case.offset + 1) +
                              "): invalid JSON — expected a value, found '" + test_case.found + "' at \"" +
                              std::string(test_case.path) + "\"";
        EXPECT_NE(message.find(expected), std::string::npos) << message;
        EXPECT_EQ(message.find("could not read"), std::string::npos) << message;
        EXPECT_EQ(message.find(std::string(test_case.path) + "["), std::string::npos) << message;
        EXPECT_EQ(message.find("expects"), std::string::npos) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_TrailingCommaInObject_KeepsQuotedKeySyntaxReason) {
    // The object side already classifies a trailing comma correctly: expecting a
    // key and finding '}' is invalid JSON, so no missing-value fact is needed.
    const auto message = load_failure_message(R"({"drivers":{"load":["a"],}})");
    EXPECT_NE(message.find("(line 1, column 26): invalid JSON — expected a quoted key"), std::string::npos) << message;
    EXPECT_EQ(message.find("expected a value"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, LoadConfig_GarbageTokenWhereValueExpected_ReportsUnexpectedCharacter) {
    // A token that can never start a JSON value is unconditional syntax damage:
    // the framing must stay invalid JSON regardless of which reader rejected it,
    // and the message must not hint at the offending character (value contract).
    struct Case {
        std::string_view content;
        glz::error_code code;
        std::size_t offset;
        std::size_t column;
        std::string_view path;
    };

    const Case cases[] = {
        {R"({"drivers":{"load":["a", x]}})", glz::error_code::expected_quote, 25, 26, "drivers.load[1]"},
        {R"({"drivers":{"load":[x]}})", glz::error_code::expected_quote, 20, 21, "drivers.load[0]"},
        {R"({"domains":[x]})", glz::error_code::expected_brace, 12, 13, "domains[0]"},
        {R"({"domains":[{},x]})", glz::error_code::expected_brace, 15, 16, "domains[1]"},
        {R"({"drivers":{"auto_discover": x}})", glz::error_code::expected_true_or_false, 29, 30,
         "drivers.auto_discover"},
        {R"({"resolver":{"strategy": @}})", glz::error_code::expected_quote, 25, 26, "resolver.strategy"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.content);
        Config::AppConfig config;
        const auto error = glz::read_json(config, test_case.content);
        SCOPED_TRACE("Glaze code=" + glz::format_error(error.ec) + ", offset=" + std::to_string(error.count));
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, test_case.code);
        EXPECT_EQ(error.count, test_case.offset);
        const auto message = load_failure_message(test_case.content);
        const auto expected = "(line 1, column " + std::to_string(test_case.column) +
                              "): invalid JSON — an unexpected character at \"" + std::string(test_case.path) + "\"";
        EXPECT_NE(message.find(expected), std::string::npos) << message;
        EXPECT_EQ(message.find("could not read"), std::string::npos) << message;
        EXPECT_EQ(message.find("expects"), std::string::npos) << message;
        // Nothing trails the path: the offending character is never echoed.
        EXPECT_TRUE(message.ends_with(expected)) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_MissingMemberValueInObject_ReportsMissingValue) {
    // A separator or closing brace right after ':' is unconditional syntax
    // damage. Some readers report past the offending token, so detection must
    // not rely on the scan stopping there.
    struct Case {
        std::string_view content;
        glz::error_code code;
        std::size_t offset;
        char found;
        std::size_t column;
        std::string_view path;
    };

    const Case cases[] = {
        {R"({"drivers":{"auto_discover":,}})", glz::error_code::expected_true_or_false, 28, ',', 29,
         "drivers.auto_discover"},
        {R"({"drivers":{"auto_discover":}})", glz::error_code::expected_true_or_false, 28, '}', 29,
         "drivers.auto_discover"},
        {R"({"resolver":{"strategy":}})", glz::error_code::expected_quote, 24, '}', 25, "resolver.strategy"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.content);
        Config::AppConfig config;
        const auto error = glz::read_json(config, test_case.content);
        SCOPED_TRACE("Glaze code=" + glz::format_error(error.ec) + ", offset=" + std::to_string(error.count));
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, test_case.code);
        EXPECT_EQ(error.count, test_case.offset);
        const auto message = load_failure_message(test_case.content);
        const auto expected = "(line 1, column " + std::to_string(test_case.column) +
                              "): invalid JSON — expected a value, found '" + test_case.found + "' at \"" +
                              std::string(test_case.path) + "\"";
        EXPECT_NE(message.find(expected), std::string::npos) << message;
        EXPECT_EQ(message.find("could not read"), std::string::npos) << message;
        EXPECT_EQ(message.find("expects"), std::string::npos) << message;
    }
}

TEST(ConfigLoaderTest, LoadConfig_EscapedKnownKey_PreservesGlazeRejectionAndNamesDecodedKey) {
    Config::AppConfig literal_config;
    ASSERT_FALSE(glz::read_json(literal_config, R"({"drivers":{"load":[]}})"));
    const std::string_view content = R"({"drivers":{"\u006coad":[]}})";
    Config::AppConfig config;
    const auto error = glz::read_json(config, content);
    SCOPED_TRACE("Glaze code=" + glz::format_error(error.ec) + ", offset=" + std::to_string(error.count));
    ASSERT_TRUE(error);
    // Glaze matches reflected member names literally, even though the spelling is valid JSON.
    EXPECT_EQ(error.ec, glz::error_code::unknown_key);
    EXPECT_EQ(error.count, 13U);
    const auto message = load_failure_message(content);
    EXPECT_NE(message.find("unknown key \"load\" in \"drivers\""), std::string::npos) << message;
    EXPECT_NE(message.find("(line 1, column 13):"), std::string::npos) << message;
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, LoadConfig_UnicodeAndEscapedUnknownKeys_NamesDecodedKeyAndParent) {
    struct Case {
        std::string_view key;
        std::string_view decoded_key;
    };

    const Case cases[] = {{"域名", "域名"},
                          {R"(\u57df\u540d)", "域名"},
                          {R"(driver\u005fdire)", "driver_dire"},
                          {R"(quo\"te)", "quo\"te"}};
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.key);
        const std::string content = "{\"drivers\":{\"" + std::string(test_case.key) + "\":1}}";
        Config::AppConfig config;
        const auto error = glz::read_json(config, content);
        ASSERT_TRUE(error);
        EXPECT_EQ(error.ec, glz::error_code::unknown_key);
        const auto message = load_failure_message(content);
        EXPECT_NE(message.find("unknown key \"" + std::string(test_case.decoded_key) + "\" in \"drivers\""),
                  std::string::npos)
            << message;
        EXPECT_NE(message.find("(line 1, column 13):"), std::string::npos) << message;
        if (test_case.decoded_key == "driver_dire") {
            EXPECT_NE(message.find("did you mean \"driver_dir\""), std::string::npos) << message;
        }
    }
}

TEST(ConfigLoaderTest, LoadConfig_UnicodeBeforeFailure_UsesByteColumns) {
    const auto message = load_failure_message(R"({"drivers":{"load":["域名",2]}})");

    EXPECT_NE(message.find("\"drivers.load[1]\" expects a string"), std::string::npos) << message;
    EXPECT_NE(message.find("(line 1, column 30):"), std::string::npos) << message;
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, DescribeParseError_LargeUnrelatedSuffix_PreservesEarlierDiagnostic) {
    const std::string prefix = R"({"drivers":{"load":["one",2)";
    const auto offset = prefix.size() - 1;
    const auto expected =
        Config::Diagnostic::describe_parse_error("suffix.json", prefix, offset, glz::error_code::expected_quote);
    ASSERT_NE(expected.find("\"drivers.load[1]\" expects a string"), std::string::npos) << expected;
    for (const auto* suffix : {"]}}", "\\q\",\"nope\":", "[{"}) {
        std::string content = prefix;
        content.append(1024 * 1024, ' ');
        content += suffix;
        EXPECT_EQ(
            Config::Diagnostic::describe_parse_error("suffix.json", content, offset, glz::error_code::expected_quote),
            expected);
    }
}

TEST(ConfigLoaderTest, DescribeParseError_DeepNestedInput_ReportsCompleteNonzeroIndexPath) {
    // Exercise the diagnostic scanner directly, independently of Glaze's recursion limit.
    constexpr std::size_t DEPTH = 1024;
    std::string content = R"({"domains":[{},{"subdomains":[{},)";
    std::string path = "domains[1].subdomains[1]";
    for (std::size_t i = 0; i < DEPTH; ++i) {
        content += R"({"children":[{},)";
        path += ".children[1]";
    }
    content += R"({"allow_ula":)";
    const auto offset = content.size();
    content += "7}";
    for (std::size_t i = 0; i < DEPTH; ++i) {
        content += "]}";
    }
    content += "]}]}";
    path += ".allow_ula";
    const auto message =
        Config::Diagnostic::describe_parse_error("deep.json", content, offset, glz::error_code::expected_true_or_false);
    EXPECT_NE(message.find("\"" + path + "\""), std::string::npos) << message;
    EXPECT_NE(message.find("(line 1, column " + std::to_string(offset + 1) + "):"), std::string::npos) << message;
    EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
}

TEST(ConfigLoaderTest, EmptyFile_ThrowsRuntimeError) {
    auto path = write_temp_config("");

    EXPECT_THROW({ [[maybe_unused]] auto cfg = Config::load_config(path); }, std::runtime_error);

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, MissingRequiredField_ThrowsRuntimeError) {
    // Missing "resolver" section.
    auto path = write_temp_config(R"({"drivers": {"directory": "/x", "load": []}})");

    EXPECT_THROW({ [[maybe_unused]] auto cfg = Config::load_config(path); }, std::runtime_error);

    std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, ConfigFields_ArePopulated) {
    auto path = write_temp_config(get_minimal_valid_config());

    auto cfg = Config::load_config(path);
    EXPECT_FALSE(cfg.resolver.use_custom_servers);
    EXPECT_TRUE(cfg.domains.empty());
    EXPECT_TRUE(cfg.drivers.load.empty());

    std::filesystem::remove(path);
}

// ── Diagnostic component contracts ─────────────────────────────────────────

TEST(DiagnosticLocatorTest, Locate_WhitespaceBeforeNestedArrayValue_ReturnsExactComponentsAndBytePosition) {
    using namespace Config::Diagnostic;
    const std::string_view buffer = "{\"a.b[0]\":[{}, {\"items\":[\"域名\",\n  true]}]}";
    const auto site = locate(buffer, buffer.find('\n'));

    EXPECT_EQ(site.line, 2U);
    EXPECT_EQ(site.column, 3U);
    EXPECT_EQ(site.kind, JsonKind::BOOLEAN);
    EXPECT_FALSE(site.at_key);
    ASSERT_EQ(site.path.size(), 4U);
    EXPECT_EQ(site.path[0].key, "a.b[0]");
    EXPECT_FALSE(site.path[0].is_index);
    EXPECT_TRUE(site.path[1].is_index);
    EXPECT_EQ(site.path[1].index, 1U);
    EXPECT_EQ(site.path[2].key, "items");
    EXPECT_TRUE(site.path[3].is_index);
    EXPECT_EQ(site.path[3].index, 1U);

    const auto same_line = locate(buffer, buffer.find("true"));
    EXPECT_EQ(same_line.line, site.line);
    EXPECT_EQ(same_line.column, site.column);
    const std::string_view utf8_buffer = R"({"items":["域名",true]})";
    const auto utf8_site = locate(utf8_buffer, utf8_buffer.find("true"));
    EXPECT_EQ(utf8_site.column, utf8_buffer.find("true") + 1);
}

TEST(DiagnosticLocatorTest, Locate_KeyOffsetAfterQuotePolicy_DecodesKeyAndDistinguishesColon) {
    using namespace Config::Diagnostic;
    for (const bool has_colon : {false, true}) {
        SCOPED_TRACE(has_colon);
        const std::string buffer = std::string{R"({"outer":{"a\u002eb")"} + (has_colon ? ":1}}" : " 1}}");
        const auto offset = buffer.find(has_colon ? ':' : ' ', buffer.find("\\u002e"));
        const auto site = locate(buffer, offset, {.key_offset_after_quote = true});

        EXPECT_TRUE(site.at_key);
        EXPECT_EQ(site.key, "a.b");
        EXPECT_EQ(site.key_without_colon, !has_colon);
        EXPECT_FALSE(site.malformed_string);
        EXPECT_EQ(site.column, 11U);
        ASSERT_EQ(site.parent_path.size(), 1U);
        EXPECT_EQ(site.parent_path[0].key, "outer");
        ASSERT_EQ(site.path.size(), 2U);
        EXPECT_EQ(site.path[1].key, "a.b");
        EXPECT_TRUE(locate(buffer, offset).key_without_colon);
    }
}

TEST(DiagnosticLocatorTest, Locate_CompleteStringPolicy_InspectsLocalTokenNotUnrelatedSuffix) {
    using namespace Config::Diagnostic;
    for (const auto* token : {R"("bad\q")", R"("unterminated)"}) {
        const std::string buffer = std::string{R"({"custom":)"} + token;
        const auto offset = buffer.find(':') + 1;
        EXPECT_FALSE(locate(buffer, offset).malformed_string);
        const auto site = locate(buffer, offset, {.inspect_complete_string = true});
        EXPECT_TRUE(site.malformed_string);
        EXPECT_EQ(site.kind, JsonKind::STRING);
        ASSERT_EQ(site.path.size(), 1U);
        EXPECT_EQ(site.path[0].key, "custom");
    }
    const std::string_view buffer = R"({"custom":"valid","later":"bad\q"})";
    EXPECT_FALSE(locate(buffer, buffer.find(':') + 1, {.inspect_complete_string = true}).malformed_string);
}

TEST(DiagnosticLocatorTest, Locate_MalformedScalar_ReturnsTokenFactRatherThanAppSchemaExpectation) {
    using namespace Config::Diagnostic;
    for (const auto* token : {"tru", "nullx", "2x", "01", "1e+"}) {
        SCOPED_TRACE(token);
        const std::string buffer = std::string{R"({"custom":)"} + token + '}';
        const auto site = locate(buffer, buffer.find(':') + 1);
        EXPECT_EQ(site.kind, JsonKind::UNEXPECTED);
        EXPECT_TRUE(site.malformed_scalar);
        EXPECT_FALSE(site.malformed_string);
        ASSERT_EQ(site.path.size(), 1U);
        EXPECT_EQ(site.path[0].key, "custom");
    }
}

TEST(DiagnosticLocatorTest, Locate_CompletedValuesBeforeInvalidSeparatorOrEof_ReturnsContainerBoundaryFacts) {
    using namespace Config::Diagnostic;

    struct Case {
        std::string_view buffer;
        std::size_t offset;
        JsonKind container;
        Path path;
        std::size_t line;
        std::size_t column;
    };

    const Case cases[] = {
        {R"({"drivers": {"load": ["a" "b"]}})", 26, JsonKind::ARRAY, {{.key = "drivers"}, {.key = "load"}}, 1, 27},
        {R"({"drivers": {"load": ["a")", 25, JsonKind::ARRAY, {{.key = "drivers"}, {.key = "load"}}, 1, 26},
        {R"({"items":[{} {})", 13, JsonKind::ARRAY, {{.key = "items"}}, 1, 14},
        {R"({"items":[{"key":[]})", 20, JsonKind::ARRAY, {{.key = "items"}}, 1, 21},
        {"{\"items\":[true \n  false]}", 14, JsonKind::ARRAY, {{.key = "items"}}, 2, 3},
        {R"({"items":[null)", 14, JsonKind::ARRAY, {{.key = "items"}}, 1, 15},
        {R"({"items":[-1.5e+2)", 17, JsonKind::ARRAY, {{.key = "items"}}, 1, 18},
        {R"({"outer":[{}, {"items":[[], [] [])",
         30,
         JsonKind::ARRAY,
         {{.key = "outer"}, {.key = {}, .index = 1, .is_index = true}, {.key = "items"}},
         1,
         32},
        {R"({"outer":{"key":{} "next":1}})", 18, JsonKind::OBJECT, {{.key = "outer"}}, 1, 20},
        {R"({"outer":{"key":false)", 21, JsonKind::OBJECT, {{.key = "outer"}}, 1, 22},
        {R"({"key":[])", 9, JsonKind::OBJECT, {}, 1, 10},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.buffer);
        const auto site = locate(test_case.buffer, test_case.offset);
        EXPECT_EQ(site.after_value.kind, test_case.container);
        EXPECT_EQ(site.after_value.line, test_case.line);
        EXPECT_EQ(site.after_value.column, test_case.column);
        ASSERT_EQ(site.after_value.path.size(), test_case.path.size());
        for (std::size_t i = 0; i < test_case.path.size(); ++i) {
            EXPECT_EQ(site.after_value.path[i].key, test_case.path[i].key);
            EXPECT_EQ(site.after_value.path[i].index, test_case.path[i].index);
            EXPECT_EQ(site.after_value.path[i].is_index, test_case.path[i].is_index);
        }
    }
    const auto leaf = locate(cases[0].buffer, cases[0].offset);
    EXPECT_EQ(leaf.kind, JsonKind::STRING);
    EXPECT_EQ(leaf.line, 1U);
    EXPECT_EQ(leaf.column, 23U);
    ASSERT_EQ(leaf.path.size(), 3U);
    EXPECT_TRUE(leaf.path.back().is_index);
    EXPECT_EQ(leaf.path.back().index, 0U);
}

TEST(DiagnosticLocatorTest, Locate_IncompleteValueOrValidSeparator_DoesNotInventContainerBoundary) {
    using namespace Config::Diagnostic;
    for (const std::string_view buffer : {R"({"items":["unterminated)", R"({"items":[tru)", R"({"items":[1e+)",
                                          R"({"items":[{"key":)", R"({"items":[{"key":1,)", R"({"items":[)",
                                          R"({"items":["a",)", R"({"items":["a", "b"]})", R"({"items":["a"]})"}) {
        SCOPED_TRACE(buffer);
        EXPECT_EQ(locate(buffer, buffer.size()).after_value.kind, JsonKind::NONE);
    }
    const std::string_view comma = R"({"items":["a", "b"]})";
    EXPECT_EQ(locate(comma, comma.find(',')).after_value.kind, JsonKind::NONE);
    const std::string_view close = R"({"items":["a"]})";
    EXPECT_EQ(locate(close, close.find(']')).after_value.kind, JsonKind::NONE);
}

TEST(DiagnosticLocatorTest, Locate_SeparatorWhereElementExpected_ReturnsMissingValueFact) {
    using namespace Config::Diagnostic;

    struct Case {
        std::string_view buffer;
        std::size_t offset;
        char found;
        std::size_t column;
        Path path;
    };

    const Case cases[] = {
        {R"({"drivers":{"load":["a",]}})", 24, ']', 25, {{.key = "drivers"}, {.key = "load"}}},
        {R"({"drivers":{"load":["a" , ]}})", 26, ']', 27, {{.key = "drivers"}, {.key = "load"}}},
        {R"({"drivers":{"load":[,]}})", 20, ',', 21, {{.key = "drivers"}, {.key = "load"}}},
        {R"({"drivers":{"load":["a",,]}})", 24, ',', 25, {{.key = "drivers"}, {.key = "load"}}},
        {R"({"outer":[{}, {"items":["a",]}]})",
         28,
         ']',
         29,
         {{.key = "outer"}, {.key = {}, .index = 1, .is_index = true}, {.key = "items"}}},
        {R"({"items":[})", 10, '}', 11, {{.key = "items"}}},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.buffer);
        const auto site = locate(test_case.buffer, test_case.offset);
        EXPECT_EQ(site.missing_value.container, JsonKind::ARRAY);
        EXPECT_EQ(site.missing_value.found, test_case.found);
        EXPECT_EQ(site.missing_value.line, 1U);
        EXPECT_EQ(site.missing_value.column, test_case.column);
        ASSERT_EQ(site.missing_value.path.size(), test_case.path.size());
        for (std::size_t i = 0; i < test_case.path.size(); ++i) {
            EXPECT_EQ(site.missing_value.path[i].key, test_case.path[i].key);
            EXPECT_EQ(site.missing_value.path[i].index, test_case.path[i].index);
            EXPECT_EQ(site.missing_value.path[i].is_index, test_case.path[i].is_index);
        }
    }
}

TEST(DiagnosticLocatorTest, Locate_EmptyArrayEofOrRealElement_DoesNotInventMissingValue) {
    using namespace Config::Diagnostic;
    // A cleanly closed empty array is not missing a value.
    const std::string_view empty = R"({"items":[])";
    EXPECT_EQ(locate(empty, empty.find(']')).missing_value.container, JsonKind::NONE);
    // EOF right after a separator is covered by the end-of-input diagnosis.
    const std::string_view eof = R"({"items":["a",)";
    EXPECT_EQ(locate(eof, eof.size()).missing_value.container, JsonKind::NONE);
    // Whitespace before a real element still names that element, not a comma.
    const std::string_view spaced = R"({"items":[ true]})";
    const auto site = locate(spaced, spaced.find(' '));
    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
    EXPECT_EQ(site.kind, JsonKind::BOOLEAN);
    // A valid separator between elements is not a stray one.
    const std::string_view valid = R"({"items":["a", "b"]})";
    EXPECT_EQ(locate(valid, valid.find(',')).missing_value.container, JsonKind::NONE);
    // A real member value after ':' is present, whatever follows it.
    const std::string_view number = R"({"outer":{"key": 1}})";
    const auto member = locate(number, number.find(": ") + 2);
    EXPECT_EQ(member.missing_value.container, JsonKind::NONE);
    EXPECT_EQ(member.kind, JsonKind::NUMBER);
}

TEST(DiagnosticLocatorTest, Locate_SeparatorWhereMemberValueExpected_ReturnsObjectMissingValueFact) {
    using namespace Config::Diagnostic;
    // The fact must appear whether the reader stopped at the offending token
    // (lookahead) or consumed it before giving up (past-the-token offsets).

    struct Case {
        std::string_view buffer;
        std::size_t offset;
        char found;
    };

    const Case cases[] = {
        {R"({"outer":{"key":,}})", 16, ','}, {R"({"outer":{"key":,}})", 20, ','}, {R"({"outer":{"key":}})", 16, '}'},
        {R"({"outer":{"key":}})", 19, '}'},  {R"({"outer":{"key":]}})", 17, ']'}, {R"({"outer":{"key":]}})", 20, ']'},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.buffer);
        SCOPED_TRACE(test_case.offset);
        const auto site = locate(test_case.buffer, test_case.offset);
        EXPECT_EQ(site.missing_value.container, JsonKind::OBJECT);
        EXPECT_EQ(site.missing_value.found, test_case.found);
        EXPECT_EQ(site.missing_value.line, 1U);
        EXPECT_EQ(site.missing_value.column, 17U);
        ASSERT_EQ(site.missing_value.path.size(), 2U);
        EXPECT_EQ(site.missing_value.path[0].key, "outer");
        EXPECT_EQ(site.missing_value.path[1].key, "key");
    }
}

TEST(DiagnosticLocatorTest, Locate_TokenThatCannotStartAValue_ReturnsUnexpectedKindWithoutMissingValue) {
    using namespace Config::Diagnostic;
    // A garbage token is reported as the unexpected site itself, so no
    // missing-value fact may be invented for it.
    const std::string_view element = R"({"items":[x]})";
    const auto element_site = locate(element, element.find('x'));
    EXPECT_EQ(element_site.kind, JsonKind::UNEXPECTED);
    EXPECT_FALSE(element_site.malformed_scalar);
    EXPECT_EQ(element_site.column, 11U);
    ASSERT_EQ(element_site.path.size(), 2U);
    EXPECT_EQ(element_site.path[0].key, "items");
    EXPECT_TRUE(element_site.path[1].is_index);
    EXPECT_EQ(element_site.path[1].index, 0U);
    EXPECT_EQ(element_site.missing_value.container, JsonKind::NONE);

    const std::string_view member = R"({"items":@})";
    const auto member_site = locate(member, member.find('@'));
    EXPECT_EQ(member_site.kind, JsonKind::UNEXPECTED);
    EXPECT_FALSE(member_site.malformed_scalar);
    EXPECT_EQ(member_site.column, 10U);
    ASSERT_EQ(member_site.path.size(), 1U);
    EXPECT_EQ(member_site.path[0].key, "items");
    EXPECT_EQ(member_site.missing_value.container, JsonKind::NONE);
}

TEST(DiagnosticSchemaTest, ExpectationFor_IndexedAndNullablePaths_ReturnsTypeAndDeclaredConstants) {
    using namespace Config::Diagnostic;
    const auto optional_string = expectation_for({{.key = "drivers"}, {.key = "driver_dir"}});
    EXPECT_EQ(optional_string.type, JsonKind::STRING);
    EXPECT_TRUE(optional_string.nullable);
    EXPECT_TRUE(optional_string.values.empty());

    const Path subdomain{{.key = "domains"},
                         {.key = {}, .index = 7, .is_index = true},
                         {.key = "subdomains"},
                         {.key = {}, .index = 3, .is_index = true}};
    auto path = subdomain;
    path.push_back({.key = "ip_source"});
    const auto source = expectation_for(path);
    EXPECT_EQ(source.type, JsonKind::STRING);
    EXPECT_TRUE(source.nullable);
    EXPECT_THAT(source.values, testing::ElementsAre("interface", "http", "url", "mdns"));

    path.back().key = "allow_ula";
    const auto boolean = expectation_for(path);
    EXPECT_EQ(boolean.type, JsonKind::BOOLEAN);
    EXPECT_FALSE(boolean.nullable);
    EXPECT_TRUE(boolean.values.empty());
    EXPECT_EQ(expectation_for(subdomain).type, JsonKind::OBJECT);
}

TEST(DiagnosticSchemaTest, MemberNamesFor_ObjectAndUnknownPaths_ReturnsNamesWithoutGuessingPathSyntax) {
    using namespace Config::Diagnostic;
    EXPECT_THAT(member_names_for({{.key = "drivers"}}),
                testing::UnorderedElementsAre("driver_dir", "auto_discover", "load"));
    EXPECT_THAT(member_names_for({{.key = "resolver"}, {.key = "servers"}, {.key = {}, .index = 4, .is_index = true}}),
                testing::UnorderedElementsAre("address", "ipaddress", "port"));
    const Path unknown{{.key = "drivers.load[0]"}};
    const auto expected = expectation_for(unknown);
    EXPECT_EQ(expected.type, JsonKind::NONE);
    EXPECT_FALSE(expected.nullable);
    EXPECT_TRUE(expected.values.empty());
    EXPECT_TRUE(member_names_for(unknown).empty());
    EXPECT_TRUE(member_names_for({{.key = "drivers"}, {.key = "auto_discover"}}).empty());
}

TEST(DiagnosticDecisionTest, Decide_IoFailureWithEmptyAndMalformedFacts_IgnoresContentDiagnostics) {
    using namespace Config::Diagnostic;
    const Site site{.parent_path = {},
                    .key = {},
                    .kind = JsonKind::STRING,
                    .path = {},
                    .malformed_string = true,
                    .malformed_scalar = true};
    const SchemaFacts schema{.expected = {.type = JsonKind::BOOLEAN, .values = {}}, .member_names = {"custom"}};
    for (const auto kind :
         {ErrorKind::FILE_OPEN, ErrorKind::FILE_CLOSE, ErrorKind::FILE_INCLUDE, ErrorKind::FILE_EXTENSION}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto diagnosis = decide({.kind = kind, .identifier = {}}, site, {.empty = true, .at_end = true}, schema);
        EXPECT_EQ(diagnosis.failure.kind, kind);
        EXPECT_EQ(diagnosis.reason, Reason::CODE);
        EXPECT_EQ(diagnosis.framing, Framing::NONE);
        EXPECT_FALSE(diagnosis.show_position);
        EXPECT_FALSE(diagnosis.append_location);
        EXPECT_EQ(diagnosis.expected.type, JsonKind::NONE);
        EXPECT_TRUE(diagnosis.candidates.empty());
    }
}

TEST(DiagnosticDecisionTest, FactRequirements_IoOrEmptyInput_NeedsNeitherLocationNorSchemaAndMatchesDecision) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::ARRAY, .values = {}}, .member_names = {"custom"}};
    for (const auto kind : {ErrorKind::FILE_OPEN, ErrorKind::FILE_CLOSE, ErrorKind::FILE_INCLUDE,
                            ErrorKind::FILE_EXTENSION, ErrorKind::UNKNOWN_KEY, ErrorKind::ENUM}) {
        for (const bool empty : {false, true}) {
            if (!empty && (kind == ErrorKind::UNKNOWN_KEY || kind == ErrorKind::ENUM)) {
                continue;
            }
            SCOPED_TRACE(static_cast<int>(kind));
            SCOPED_TRACE(empty);
            const ParseFailure failure{.kind = kind, .identifier = {}};
            const InputFacts input{.empty = empty};
            EXPECT_FALSE(needs_location(failure, input));
            EXPECT_EQ(schema_need(failure, {}, input), SchemaNeed::NONE);
            const auto diagnosis = decide(failure, {}, input, {});
            const auto with_schema = decide(failure, {}, input, schema);
            const bool io = kind != ErrorKind::UNKNOWN_KEY && kind != ErrorKind::ENUM;
            EXPECT_EQ(diagnosis.reason, io ? Reason::CODE : Reason::EMPTY);
            EXPECT_EQ(diagnosis.show_position, !io);
            EXPECT_EQ(diagnosis.framing, Framing::NONE);
            EXPECT_EQ(with_schema.reason, diagnosis.reason);
            EXPECT_EQ(with_schema.show_position, diagnosis.show_position);
            EXPECT_EQ(with_schema.expected.type, JsonKind::NONE);
            EXPECT_TRUE(with_schema.candidates.empty());
        }
    }
}

TEST(DiagnosticDecisionTest, SchemaNeed_TokenAndSchemaRoutes_RequestsOnlyFactsUsedByDecision) {
    using namespace Config::Diagnostic;

    struct Case {
        ErrorKind failure;
        JsonKind actual;
        bool malformed_string;
        bool malformed_scalar;
        bool at_key;
        SchemaNeed need;
        Reason reason;
    };

    const Case cases[] = {
        {ErrorKind::ENUM, JsonKind::STRING, true, false, false, SchemaNeed::NONE, Reason::MALFORMED_STRING},
        {ErrorKind::UNKNOWN_KEY, JsonKind::UNEXPECTED, false, true, false, SchemaNeed::NONE, Reason::MALFORMED_SCALAR},
        {ErrorKind::EXPECTED_COMMA, JsonKind::BOOLEAN, false, false, false, SchemaNeed::NONE, Reason::CODE},
        {ErrorKind::SYNTAX, JsonKind::UNEXPECTED, false, false, false, SchemaNeed::NONE, Reason::UNEXPECTED_TOKEN},
        {ErrorKind::NUMBER, JsonKind::NUMBER, false, false, false, SchemaNeed::NONE, Reason::CODE},
        {ErrorKind::UNKNOWN_KEY, JsonKind::NONE, false, false, true, SchemaNeed::MEMBER_NAMES, Reason::UNKNOWN_KEY},
        {ErrorKind::OTHER, JsonKind::BOOLEAN, false, false, false, SchemaNeed::EXPECTATION, Reason::EXPECTED_TYPE},
        {ErrorKind::SYNTAX, JsonKind::BOOLEAN, false, false, false, SchemaNeed::EXPECTATION, Reason::EXPECTED_TYPE},
        {ErrorKind::NUMBER, JsonKind::STRING, false, false, false, SchemaNeed::EXPECTATION, Reason::EXPECTED_TYPE},
    };
    const SchemaFacts all_facts{.expected = {.type = JsonKind::ARRAY, .values = {}}, .member_names = {"custom"}};
    for (const auto& test_case : cases) {
        SCOPED_TRACE(static_cast<int>(test_case.failure));
        SCOPED_TRACE(static_cast<int>(test_case.actual));
        const ParseFailure failure{.kind = test_case.failure, .identifier = {}};
        const Site site{.parent_path = {},
                        .key = "custm",
                        .kind = test_case.actual,
                        .at_key = test_case.at_key,
                        .path = {{.key = "custm"}},
                        .malformed_string = test_case.malformed_string,
                        .malformed_scalar = test_case.malformed_scalar};
        EXPECT_TRUE(needs_location(failure, {}));
        const auto need = schema_need(failure, site, {});
        EXPECT_EQ(need, test_case.need);
        SchemaFacts required_facts{};
        if (need == SchemaNeed::EXPECTATION) {
            required_facts.expected = all_facts.expected;
        } else if (need == SchemaNeed::MEMBER_NAMES) {
            required_facts.member_names = all_facts.member_names;
        }
        const auto diagnosis = decide(failure, site, {}, required_facts);
        const auto with_all_facts = decide(failure, site, {}, all_facts);
        EXPECT_EQ(diagnosis.reason, test_case.reason);
        EXPECT_EQ(diagnosis.reason, with_all_facts.reason);
        EXPECT_EQ(diagnosis.framing, with_all_facts.framing);
        EXPECT_EQ(diagnosis.expected.type, with_all_facts.expected.type);
        EXPECT_EQ(diagnosis.candidates, with_all_facts.candidates);
        EXPECT_EQ(diagnosis.suggestion, with_all_facts.suggestion);
        EXPECT_EQ(diagnosis.show_actual_kind, with_all_facts.show_actual_kind);
        if (need == SchemaNeed::MEMBER_NAMES) {
            EXPECT_EQ(diagnosis.suggestion, "custom");
            EXPECT_EQ(diagnosis.expected.type, JsonKind::NONE);
        } else if (need == SchemaNeed::EXPECTATION) {
            EXPECT_EQ(diagnosis.expected.type, JsonKind::ARRAY);
            EXPECT_TRUE(diagnosis.candidates.empty());
        }
    }
}

TEST(DiagnosticDecisionTest, Decide_SyntaxAndMalformedFacts_TakePriorityOverInjectedSchema) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::STRING, .values = {"only"}}, .member_names = {"custom"}};
    const Site site{.parent_path = {}, .key = {}, .kind = JsonKind::BOOLEAN, .path = {{.key = "custom"}}};
    for (const auto kind : {ErrorKind::EXPECTED_COMMA, ErrorKind::INVALID_ESCAPE}) {
        const auto diagnosis = decide({.kind = kind, .identifier = {}}, site, {}, schema);
        EXPECT_EQ(diagnosis.reason, Reason::CODE);
        EXPECT_EQ(diagnosis.framing, Framing::INVALID_JSON);
        EXPECT_EQ(diagnosis.expected.type, JsonKind::NONE);
    }
    auto malformed = site;
    malformed.malformed_string = true;
    EXPECT_EQ(decide({.kind = ErrorKind::ENUM, .identifier = {}}, malformed, {}, schema).reason,
              Reason::MALFORMED_STRING);
    malformed.malformed_string = false;
    malformed.malformed_scalar = true;
    EXPECT_EQ(decide({.kind = ErrorKind::UNKNOWN_KEY, .identifier = {}}, malformed, {}, schema).reason,
              Reason::MALFORMED_SCALAR);
    EXPECT_EQ(decide({.kind = ErrorKind::ENUM, .identifier = {}}, malformed, {.empty = true}, schema).reason,
              Reason::EMPTY);
    EXPECT_EQ(decide({.kind = ErrorKind::EXPECTED_BRACE, .identifier = {}}, {}, {.at_end = true}, schema).reason,
              Reason::END);
}

TEST(DiagnosticDecisionTest, Decide_ContainerBoundary_UsesBoundaryPositionAndNeedsNoSchema) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::BOOLEAN, .values = {"unused"}}, .member_names = {"unused"}};
    for (const auto container : {JsonKind::ARRAY, JsonKind::OBJECT}) {
        for (const auto kind : {ErrorKind::EXPECTED_BRACKET, ErrorKind::EXPECTED_BRACE, ErrorKind::UNEXPECTED_END}) {
            for (const bool at_end : {false, true}) {
                SCOPED_TRACE(static_cast<int>(container));
                SCOPED_TRACE(static_cast<int>(kind));
                SCOPED_TRACE(at_end);
                const ParseFailure failure{.kind = kind, .identifier = {}};
                const Site site{.line = 2,
                                .column = 5,
                                .parent_path = {},
                                .key = {},
                                .kind = JsonKind::STRING,
                                .path = {{.key = "items"}, {.key = {}, .index = 2, .is_index = true}},
                                .after_value = {.kind = container, .path = {{.key = "items"}}, .line = 4, .column = 9}};
                const InputFacts input{.at_end = at_end};
                EXPECT_TRUE(needs_location(failure, input));
                EXPECT_EQ(schema_need(failure, site, input), SchemaNeed::NONE);
                const auto diagnosis = decide(failure, site, input, {});
                const auto with_schema = decide(failure, site, input, schema);
                EXPECT_EQ(diagnosis.reason, container == JsonKind::ARRAY ? Reason::ARRAY_SEPARATOR_OR_CLOSE
                                                                         : Reason::OBJECT_SEPARATOR_OR_CLOSE);
                EXPECT_EQ(with_schema.reason, diagnosis.reason);
                EXPECT_EQ(diagnosis.framing, Framing::INVALID_JSON);
                EXPECT_EQ(with_schema.framing, diagnosis.framing);
                EXPECT_EQ(diagnosis.site.line, 4U);
                EXPECT_EQ(diagnosis.site.column, 9U);
                EXPECT_THAT(diagnosis.site.path, testing::ElementsAre(testing::Field(&PathComponent::key, "items")));
                EXPECT_EQ(diagnosis.failure.kind, kind);
                EXPECT_TRUE(diagnosis.show_position);
                EXPECT_TRUE(diagnosis.append_location);
                EXPECT_FALSE(diagnosis.show_actual_kind);
                EXPECT_EQ(with_schema.expected.type, JsonKind::NONE);
                EXPECT_TRUE(with_schema.expected.values.empty());
                EXPECT_TRUE(with_schema.candidates.empty());
                EXPECT_TRUE(with_schema.suggestion.empty());
                auto root = site;
                root.after_value.path.clear();
                const auto root_diagnosis = decide(failure, root, input, schema);
                EXPECT_TRUE(root_diagnosis.site.path.empty());
                EXPECT_FALSE(root_diagnosis.append_location);
                auto no_leaf = site;
                no_leaf.kind = JsonKind::NONE;
                EXPECT_EQ(schema_need(failure, no_leaf, {.at_end = true}), SchemaNeed::NONE);
                EXPECT_EQ(decide(failure, no_leaf, {.at_end = true}, schema).reason, diagnosis.reason);
            }
        }
    }
}

TEST(DiagnosticDecisionTest, Decide_TypeOrEnumRejectionWithCompletedValue_DoesNotUseBoundary) {
    using namespace Config::Diagnostic;
    for (const bool enumeration : {false, true}) {
        SCOPED_TRACE(enumeration);
        const std::string_view buffer =
            enumeration ? R"({"resolver":{"strategy":"invalid")" : R"({"drivers":{"load":[true)";
        const auto site = locate(buffer, buffer.size(), {.inspect_complete_string = true});
        ASSERT_NE(site.after_value.kind, JsonKind::NONE);
        const ParseFailure failure{.kind = enumeration ? ErrorKind::ENUM : ErrorKind::OTHER, .identifier = {}};
        const SchemaFacts schema{
            .expected = {.type = JsonKind::STRING,
                         .values = enumeration ? std::vector<std::string>{"fallback", "concurrent", "shuffle"}
                                               : std::vector<std::string>{}},
            .member_names = {}};
        EXPECT_EQ(schema_need(failure, site, {.at_end = true}), SchemaNeed::EXPECTATION);
        const auto diagnosis = decide(failure, site, {.at_end = true}, schema);
        EXPECT_EQ(diagnosis.reason, enumeration ? Reason::EXPECTED_CONSTANT : Reason::EXPECTED_TYPE);
        EXPECT_EQ(diagnosis.framing, Framing::NONE);
        EXPECT_EQ(diagnosis.site.line, site.line);
        EXPECT_EQ(diagnosis.site.column, site.column);
        EXPECT_EQ(diagnosis.site.path.size(), site.path.size());
        EXPECT_EQ(diagnosis.show_actual_kind, !enumeration);
        EXPECT_FALSE(diagnosis.append_location);
        const auto message = render("rejected.json", diagnosis);
        EXPECT_NE(message.find(enumeration ? "\"resolver.strategy\" expects one of"
                                           : "\"drivers.load[0]\" expects a string, got true or false"),
                  std::string::npos)
            << message;
        EXPECT_EQ(message.find("invalid JSON"), std::string::npos) << message;
        EXPECT_EQ(message.find("after an"), std::string::npos) << message;
    }
}

TEST(DiagnosticDecisionTest, Decide_MalformedTokenWithBoundaryFacts_PreservesTokenFailurePriority) {
    using namespace Config::Diagnostic;
    for (const bool string : {false, true}) {
        const Site site{.parent_path = {},
                        .key = {},
                        .path = {},
                        .malformed_string = string,
                        .malformed_scalar = !string,
                        .after_value = {.kind = JsonKind::ARRAY, .path = {{.key = "items"}}}};
        const ParseFailure failure{.kind = ErrorKind::UNEXPECTED_END, .identifier = {}};
        EXPECT_EQ(schema_need(failure, site, {.at_end = true}), SchemaNeed::NONE);
        EXPECT_EQ(decide(failure, site, {.at_end = true}, {}).reason,
                  string ? Reason::MALFORMED_STRING : Reason::MALFORMED_SCALAR);
    }
}

TEST(DiagnosticDecisionTest, Decide_MissingValueFact_OverridesReaderCodeAndNeedsNoSchema) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::STRING, .values = {"unused"}}, .member_names = {"unused"}};
    for (const auto kind : {ErrorKind::EXPECTED_QUOTE, ErrorKind::EXPECTED_BRACE, ErrorKind::EXPECTED_BRACKET,
                            ErrorKind::UNEXPECTED_END}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const ParseFailure failure{.kind = kind, .identifier = {}};
        const Site site{.line = 99,
                        .column = 88,
                        .parent_path = {},
                        .key = {},
                        .path = {},
                        .missing_value = {.container = JsonKind::ARRAY,
                                          .path = {{.key = "drivers"}, {.key = "load"}},
                                          .line = 1,
                                          .column = 25,
                                          .found = ']'}};
        EXPECT_TRUE(needs_location(failure, {}));
        EXPECT_EQ(schema_need(failure, site, {}), SchemaNeed::NONE);
        const auto diagnosis = decide(failure, site, {}, schema);
        EXPECT_EQ(diagnosis.reason, Reason::MISSING_VALUE);
        EXPECT_EQ(diagnosis.framing, Framing::INVALID_JSON);
        EXPECT_EQ(diagnosis.site.line, 1U);
        EXPECT_EQ(diagnosis.site.column, 25U);
        EXPECT_THAT(diagnosis.site.path, testing::ElementsAre(testing::Field(&PathComponent::key, "drivers"),
                                                              testing::Field(&PathComponent::key, "load")));
        EXPECT_TRUE(diagnosis.show_position);
        EXPECT_TRUE(diagnosis.append_location);
        EXPECT_FALSE(diagnosis.show_actual_kind);
        EXPECT_TRUE(diagnosis.expected.values.empty());
        EXPECT_TRUE(diagnosis.candidates.empty());
        EXPECT_TRUE(diagnosis.suggestion.empty());
    }
    // A member value missing after ':' takes the same route as the array side.
    const ParseFailure failure{.kind = ErrorKind::EXPECTED_QUOTE, .identifier = {}};
    const Site object_site{.parent_path = {},
                           .key = {},
                           .path = {},
                           .missing_value = {.container = JsonKind::OBJECT,
                                             .path = {{.key = "drivers"}, {.key = "auto_discover"}},
                                             .line = 1,
                                             .column = 29,
                                             .found = ','}};
    EXPECT_TRUE(needs_location(failure, {}));
    EXPECT_EQ(schema_need(failure, object_site, {}), SchemaNeed::NONE);
    const auto object = decide(failure, object_site, {}, schema);
    EXPECT_EQ(object.reason, Reason::MISSING_VALUE);
    EXPECT_EQ(object.framing, Framing::INVALID_JSON);
    EXPECT_EQ(object.site.line, 1U);
    EXPECT_EQ(object.site.column, 29U);
    EXPECT_THAT(object.site.path, testing::ElementsAre(testing::Field(&PathComponent::key, "drivers"),
                                                       testing::Field(&PathComponent::key, "auto_discover")));
    EXPECT_TRUE(object.show_position);
    EXPECT_TRUE(object.append_location);
    EXPECT_FALSE(object.show_actual_kind);
}

TEST(DiagnosticDecisionTest, Decide_UnexpectedTokenAtValuePosition_OverridesReaderCodeAndNeedsNoSchema) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::STRING, .values = {"unused"}}, .member_names = {"unused"}};
    const Site site{.line = 1,
                    .column = 26,
                    .parent_path = {},
                    .key = {},
                    .kind = JsonKind::UNEXPECTED,
                    .path = {{.key = "drivers"}, {.key = "load"}, {.key = {}, .index = 1, .is_index = true}}};
    for (const auto kind : {ErrorKind::EXPECTED_QUOTE, ErrorKind::EXPECTED_BRACE, ErrorKind::SYNTAX, ErrorKind::NUMBER,
                            ErrorKind::OTHER}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const ParseFailure failure{.kind = kind, .identifier = {}};
        EXPECT_TRUE(needs_location(failure, {}));
        EXPECT_EQ(schema_need(failure, site, {}), SchemaNeed::NONE);
        const auto diagnosis = decide(failure, site, {}, schema);
        EXPECT_EQ(diagnosis.reason, Reason::UNEXPECTED_TOKEN);
        EXPECT_EQ(diagnosis.framing, Framing::INVALID_JSON);
        EXPECT_EQ(diagnosis.site.line, 1U);
        EXPECT_EQ(diagnosis.site.column, 26U);
        EXPECT_THAT(diagnosis.site.path, testing::ElementsAre(testing::Field(&PathComponent::key, "drivers"),
                                                              testing::Field(&PathComponent::key, "load"),
                                                              testing::Field(&PathComponent::index, 1U)));
        EXPECT_TRUE(diagnosis.show_position);
        EXPECT_TRUE(diagnosis.append_location);
        EXPECT_FALSE(diagnosis.show_actual_kind);
        EXPECT_TRUE(diagnosis.expected.values.empty());
        EXPECT_TRUE(diagnosis.candidates.empty());
        EXPECT_TRUE(diagnosis.suggestion.empty());
    }
    // A token that started a value but broke mid-way stays a malformed scalar.
    const Site malformed{.parent_path = {},
                         .key = {},
                         .kind = JsonKind::UNEXPECTED,
                         .path = {{.key = "drivers"}, {.key = "load"}},
                         .malformed_scalar = true};
    const auto broken = decide({.kind = ErrorKind::EXPECTED_QUOTE, .identifier = {}}, malformed, {}, schema);
    EXPECT_EQ(broken.reason, Reason::MALFORMED_SCALAR);
}

TEST(DiagnosticDecisionTest, Decide_InjectedTypeFacts_ExplainsMismatchWithoutLookingUpAppSchema) {
    using namespace Config::Diagnostic;
    const Site site{.parent_path = {}, .key = {}, .kind = JsonKind::BOOLEAN, .path = {{.key = "not_an_app_property"}}};
    const SchemaFacts schema{.expected = {.type = JsonKind::ARRAY, .values = {}}, .member_names = {}};
    for (const auto kind : {ErrorKind::SYNTAX, ErrorKind::NUMBER, ErrorKind::OTHER}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto diagnosis = decide({.kind = kind, .identifier = {}}, site, {}, schema);
        EXPECT_EQ(diagnosis.reason, Reason::EXPECTED_TYPE);
        EXPECT_EQ(diagnosis.framing, Framing::NONE);
        EXPECT_EQ(diagnosis.expected.type, JsonKind::ARRAY);
        EXPECT_TRUE(diagnosis.show_actual_kind);
        EXPECT_FALSE(diagnosis.append_location);
    }
}

TEST(DiagnosticDecisionTest, Decide_CompatibleNumericOrNullableFacts_DoesNotInventTypeMismatch) {
    using namespace Config::Diagnostic;
    for (const auto kind : {JsonKind::NUMBER, JsonKind::NULL_VALUE}) {
        const Site site{.parent_path = {}, .key = {}, .kind = kind, .path = {}};
        const SchemaFacts schema{.expected = {.type = JsonKind::INTEGER, .values = {}, .nullable = true},
                                 .member_names = {}};
        const auto diagnosis = decide({.kind = ErrorKind::CONSTRAINT, .identifier = {}}, site, {}, schema);
        EXPECT_EQ(diagnosis.reason, Reason::CODE);
        EXPECT_EQ(diagnosis.framing, Framing::READ_FAILURE);
        EXPECT_FALSE(diagnosis.show_actual_kind);
    }
    EXPECT_EQ(decide({.kind = ErrorKind::INVALID_NULL, .identifier = {}},
                     {.parent_path = {}, .key = {}, .kind = JsonKind::NULL_VALUE, .path = {}}, {},
                     {.expected = {.type = JsonKind::INTEGER, .values = {}}, .member_names = {}})
                  .reason,
              Reason::EXPECTED_TYPE);
}

TEST(DiagnosticDecisionTest, Decide_InjectedConstants_UsesEnumFailureButNotUnrelatedStringConstraint) {
    using namespace Config::Diagnostic;
    const SchemaFacts schema{.expected = {.type = JsonKind::STRING, .values = {"first", "second"}}, .member_names = {}};
    const auto enumeration = decide({.kind = ErrorKind::ENUM, .identifier = {}},
                                    {.parent_path = {}, .key = {}, .kind = JsonKind::STRING, .path = {}}, {}, schema);
    EXPECT_EQ(enumeration.reason, Reason::EXPECTED_CONSTANT);
    EXPECT_THAT(enumeration.expected.values, testing::ElementsAre("first", "second"));
    EXPECT_FALSE(enumeration.show_actual_kind);
    const auto wrong_type = decide({.kind = ErrorKind::OTHER, .identifier = {}},
                                   {.parent_path = {}, .key = {}, .kind = JsonKind::BOOLEAN, .path = {}}, {}, schema);
    EXPECT_EQ(wrong_type.reason, Reason::EXPECTED_CONSTANT);
    EXPECT_TRUE(wrong_type.show_actual_kind);
    EXPECT_EQ(decide({.kind = ErrorKind::CONSTRAINT, .identifier = {}},
                     {.parent_path = {}, .key = {}, .kind = JsonKind::STRING, .path = {}}, {}, schema)
                  .reason,
              Reason::CODE);
    EXPECT_THAT(schema.expected.values, testing::ElementsAre("first", "second"));
}

TEST(DiagnosticDecisionTest, Decide_InjectedMemberNames_SelectsUniqueSuggestionButNotTies) {
    using namespace Config::Diagnostic;
    const Site site{.parent_path = {}, .key = "cot", .at_key = true, .path = {}};
    const auto unique = decide({.kind = ErrorKind::UNKNOWN_KEY, .identifier = {}}, site, {},
                               {.expected = {}, .member_names = {"cat", "unrelated"}});
    EXPECT_EQ(unique.reason, Reason::UNKNOWN_KEY);
    EXPECT_EQ(unique.suggestion, "cat");
    EXPECT_THAT(unique.candidates, testing::ElementsAre("cat", "unrelated"));
    const auto tied = decide({.kind = ErrorKind::UNKNOWN_KEY, .identifier = {}}, site, {},
                             {.expected = {}, .member_names = {"cat", "cut"}});
    EXPECT_EQ(tied.reason, Reason::UNKNOWN_KEY);
    EXPECT_TRUE(tied.suggestion.empty());
    EXPECT_THAT(tied.candidates, testing::ElementsAre("cat", "cut"));
}

TEST(DiagnosticRendererTest, Render_InjectedDiagnosis_UsesDecidedReasonAndStructuralPathOnly) {
    using namespace Config::Diagnostic;
    const Diagnosis diagnosis{
        .site = {.line = 8,
                 .column = 13,
                 .parent_path = {},
                 .key = {},
                 .kind = JsonKind::BOOLEAN,
                 .path = {{.key = "custom"}, {.key = {}, .index = 2, .is_index = true}, {.key = "mode"}},
                 .malformed_string = true},
        .failure = {.kind = ErrorKind::FILE_OPEN, .identifier = {}},
        .reason = Reason::EXPECTED_CONSTANT,
        .show_actual_kind = true,
        .expected = {.type = JsonKind::STRING, .values = {"alpha", "beta"}},
        .candidates = {},
        .suggestion = {}};
    EXPECT_EQ(render("never-opened.json", diagnosis),
              "config file \"never-opened.json\" (line 8, column 13): \"custom[2].mode\" expects one of "
              "\"alpha\", \"beta\" (got true or false)");
}

TEST(DiagnosticRendererTest, Render_InjectedPresentationFlags_ControlsFramingPositionAndLocation) {
    using namespace Config::Diagnostic;
    Diagnosis diagnosis{.site = {.line = 99, .column = 88, .parent_path = {}, .key = {}, .path = {{.key = "custom"}}},
                        .failure = {.kind = ErrorKind::OTHER, .identifier = "adapter_specific_failure"},
                        .framing = Framing::READ_FAILURE,
                        .show_position = false,
                        .append_location = true,
                        .expected = {},
                        .candidates = {},
                        .suggestion = {}};
    EXPECT_EQ(render("missing.json", diagnosis),
              "config file \"missing.json\": could not read configuration — adapter specific failure at \"custom\"");
    diagnosis.reason = Reason::MALFORMED_SCALAR;
    diagnosis.framing = Framing::INVALID_JSON;
    diagnosis.show_position = true;
    diagnosis.append_location = false;
    EXPECT_EQ(render("missing.json", diagnosis),
              "config file \"missing.json\" (line 99, column 88): invalid JSON — malformed scalar value");
}

TEST(DiagnosticRendererTest, Render_ContainerBoundaryReasons_UsesDecidedPositionAndOptionalContainerPath) {
    using namespace Config::Diagnostic;
    for (const auto reason : {Reason::ARRAY_SEPARATOR_OR_CLOSE, Reason::OBJECT_SEPARATOR_OR_CLOSE}) {
        Diagnosis diagnosis{
            .site = {.line = 3,
                     .column = 7,
                     .parent_path = {},
                     .key = {},
                     .path = {{.key = "outer"}, {.key = {}, .index = 2, .is_index = true}, {.key = "items"}},
                     .after_value = {.kind = JsonKind::NONE, .path = {}, .line = 99, .column = 88}},
            .failure = {.kind = ErrorKind::OTHER, .identifier = "unused"},
            .reason = reason,
            .framing = Framing::INVALID_JSON,
            .append_location = true,
            .expected = {},
            .candidates = {},
            .suggestion = {}};
        const std::string prefix = "config file \"boundary.json\" (line 3, column 7): invalid JSON — ";
        const std::string text = reason == Reason::ARRAY_SEPARATOR_OR_CLOSE
                                     ? "expected ',' or ']' after an array element"
                                     : "expected ',' or '}' after an object member";
        EXPECT_EQ(render("boundary.json", diagnosis), prefix + text + " at \"outer[2].items\"");
        diagnosis.site.path.clear();
        diagnosis.append_location = false;
        EXPECT_EQ(render("boundary.json", diagnosis), prefix + text);
    }
}

TEST(DiagnosticRendererTest, Render_MissingValueReason_NamesFoundTokenAndContainerPath) {
    using namespace Config::Diagnostic;
    Diagnosis diagnosis{.site = {.line = 1,
                                 .column = 25,
                                 .parent_path = {},
                                 .key = {},
                                 .path = {{.key = "drivers"}, {.key = "load"}},
                                 .missing_value = {.container = JsonKind::ARRAY, .path = {}, .found = ']'}},
                        .failure = {.kind = ErrorKind::EXPECTED_QUOTE, .identifier = {}},
                        .reason = Reason::MISSING_VALUE,
                        .framing = Framing::INVALID_JSON,
                        .append_location = true,
                        .expected = {},
                        .candidates = {},
                        .suggestion = {}};
    EXPECT_EQ(render("trailing.json", diagnosis),
              "config file \"trailing.json\" (line 1, column 25): invalid JSON — expected a value, found ']' at "
              "\"drivers.load\"");
    diagnosis.site.path.clear();
    diagnosis.append_location = false;
    EXPECT_EQ(render("trailing.json", diagnosis),
              "config file \"trailing.json\" (line 1, column 25): invalid JSON — expected a value, found ']'");
}

TEST(DiagnosticRendererTest, Render_UnexpectedTokenReason_UsesDecidedPositionAndPath) {
    using namespace Config::Diagnostic;
    // The offending character is never echoed: input content stays out of the message.
    Diagnosis diagnosis{
        .site = {.line = 1,
                 .column = 26,
                 .parent_path = {},
                 .key = {},
                 .kind = JsonKind::UNEXPECTED,
                 .path = {{.key = "drivers"}, {.key = "load"}, {.key = {}, .index = 1, .is_index = true}}},
        .failure = {.kind = ErrorKind::EXPECTED_QUOTE, .identifier = {}},
        .reason = Reason::UNEXPECTED_TOKEN,
        .framing = Framing::INVALID_JSON,
        .append_location = true,
        .expected = {},
        .candidates = {},
        .suggestion = {}};
    EXPECT_EQ(render("garbage.json", diagnosis),
              "config file \"garbage.json\" (line 1, column 26): invalid JSON — an unexpected character at "
              "\"drivers.load[1]\"");
    diagnosis.site.path.clear();
    diagnosis.append_location = false;
    EXPECT_EQ(render("garbage.json", diagnosis),
              "config file \"garbage.json\" (line 1, column 26): invalid JSON — an unexpected character");
}

TEST(DiagnosticErrorAdapterTest, AdaptError_LibraryBoundary_ReturnsInternalKindsAndScanningPolicies) {
    using namespace Config::Diagnostic;
    const auto unknown = adapt_error(glz::error_code::unknown_key);
    EXPECT_EQ(unknown.kind, ErrorKind::UNKNOWN_KEY);
    EXPECT_TRUE(unknown.identifier.empty());
    EXPECT_TRUE(scan_policy(unknown).key_offset_after_quote);
    EXPECT_FALSE(scan_policy(unknown).inspect_complete_string);
    const auto enumeration = adapt_error(glz::error_code::unexpected_enum);
    EXPECT_EQ(enumeration.kind, ErrorKind::ENUM);
    EXPECT_FALSE(scan_policy(enumeration).key_offset_after_quote);
    EXPECT_TRUE(scan_policy(enumeration).inspect_complete_string);
    const auto syntax = adapt_error(glz::error_code::expected_comma);
    EXPECT_EQ(syntax.kind, ErrorKind::EXPECTED_COMMA);
    EXPECT_FALSE(scan_policy(syntax).key_offset_after_quote);
    EXPECT_FALSE(scan_policy(syntax).inspect_complete_string);
    EXPECT_EQ(adapt_error(glz::error_code::missing_key).kind, ErrorKind::MISSING_KEY);
    EXPECT_EQ(adapt_error(glz::error_code::key_not_found).kind, ErrorKind::MISSING_KEY);
    const auto fallback = adapt_error(glz::error_code::expected_true_or_false);
    EXPECT_EQ(fallback.kind, ErrorKind::OTHER);
    EXPECT_EQ(fallback.identifier, "expected_true_or_false");
}
