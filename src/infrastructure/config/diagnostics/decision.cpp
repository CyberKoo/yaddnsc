#include "decision.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Config::Diagnostic {
namespace {
/// Levenshtein distance; small inputs only (configuration key names).
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> previous(b.size() + 1);
    std::vector<std::size_t> current(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U);
            current[j] = std::min({substitution, previous[j] + 1, current[j - 1] + 1});
        }
        previous.swap(current);
    }
    return previous[b.size()];
}

/// Closest candidate to @p name, or "" when no candidate is close or two tie.
[[nodiscard]] std::string closest_key(const std::string& name, const std::vector<std::string>& candidates) {
    const std::size_t limit = (std::max) (std::size_t{2}, name.size() / 3);
    std::string best;
    std::size_t best_distance = limit + 1;
    bool tied{false};
    for (const auto& candidate : candidates) {
        const auto distance = edit_distance(name, candidate);
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
            tied = false;
        } else if (distance == best_distance) {
            tied = true;
        }
    }
    return (best_distance <= limit && !tied) ? best : std::string{};
}

/// Whether the code means the text itself is not well-formed JSON.
[[nodiscard]] bool is_syntax_error(ErrorKind code) {
    switch (code) {
        case ErrorKind::EXPECTED_COMMA:
        case ErrorKind::EXPECTED_COLON:
        case ErrorKind::UNEXPECTED_END:
        case ErrorKind::SYNTAX:
        case ErrorKind::NUMBER:
        case ErrorKind::INVALID_ESCAPE:
        case ErrorKind::INCOMPLETE_UNICODE:
        case ErrorKind::INVALID_UNICODE:
        case ErrorKind::EMPTY:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool is_file_error(ErrorKind code) noexcept {
    return code == ErrorKind::FILE_OPEN || code == ErrorKind::FILE_CLOSE || code == ErrorKind::FILE_INCLUDE ||
           code == ErrorKind::FILE_EXTENSION;
}

enum class Route {
    FILE,
    EMPTY,
    END,
    MISSING_VALUE,
    UNEXPECTED_TOKEN,
    CONTAINER_BOUNDARY,
    FALLBACK,
    MEMBER_NAMES,
    EXPECTATION
};

[[nodiscard]] Route classify(const ParseFailure& failure, const Site& site, InputFacts input) {
    const auto code = failure.kind;
    if (is_file_error(code)) {
        return Route::FILE;
    }
    if (input.empty) {
        return Route::EMPTY;
    }
    if (site.malformed_string || site.malformed_scalar) {
        return Route::FALLBACK;
    }
    // A separator or closing bracket where a value is required is unconditional
    // syntax damage, independent of the reader's error code or the target type.
    if (site.missing_value.container != JsonKind::NONE) {
        return Route::MISSING_VALUE;
    }
    // A token that can never start a JSON value at a value position is
    // unconditional syntax damage, whatever the reader expected there.
    if (!site.at_key && !site.malformed_scalar && site.kind == JsonKind::UNEXPECTED) {
        return Route::UNEXPECTED_TOKEN;
    }
    // Bracket codes are ambiguous: typed readers also use them after a value.
    // Lexical completion alone must not override a type or enum rejection.
    if ((code == ErrorKind::EXPECTED_BRACKET || code == ErrorKind::EXPECTED_BRACE ||
         code == ErrorKind::UNEXPECTED_END) &&
        (site.after_value.kind == JsonKind::ARRAY || site.after_value.kind == JsonKind::OBJECT)) {
        return Route::CONTAINER_BOUNDARY;
    }
    if (input.at_end && site.kind == JsonKind::NONE &&
        (is_syntax_error(code) || code == ErrorKind::EXPECTED_QUOTE || code == ErrorKind::EXPECTED_BRACE ||
         code == ErrorKind::EXPECTED_BRACKET)) {
        return Route::END;
    }
    if (is_syntax_error(code) && !(code == ErrorKind::NUMBER && site.kind != JsonKind::NUMBER) &&
        !(code == ErrorKind::SYNTAX && site.kind != JsonKind::NONE && site.kind != JsonKind::UNEXPECTED &&
          !site.at_key)) {
        return input.at_end && code == ErrorKind::UNEXPECTED_END ? Route::END : Route::FALLBACK;
    }
    if (code == ErrorKind::UNKNOWN_KEY) {
        return Route::MEMBER_NAMES;
    }
    if (site.at_key || site.kind == JsonKind::NONE || site.kind == JsonKind::UNEXPECTED) {
        return Route::FALLBACK;
    }
    return Route::EXPECTATION;
}

void use_code_fallback(Diagnosis& diagnosis) {
    const auto& site = diagnosis.site;
    const auto code = diagnosis.failure.kind;
    diagnosis.framing = Framing::INVALID_JSON;
    if (code == ErrorKind::EXPECTED_COLON && site.at_key) {
        diagnosis.reason = Reason::MISSING_COLON;
    } else if (code == ErrorKind::EXPECTED_QUOTE && site.at_key) {
        diagnosis.reason = Reason::QUOTED_KEY;
    } else if (site.malformed_string) {
        diagnosis.reason = Reason::MALFORMED_STRING;
    } else if (site.malformed_scalar) {
        diagnosis.reason = Reason::MALFORMED_SCALAR;
    } else {
        diagnosis.reason = Reason::CODE;
        diagnosis.framing = is_syntax_error(code) ? Framing::INVALID_JSON : Framing::READ_FAILURE;
        diagnosis.append_location = !site.path.empty();
    }
}

[[nodiscard]] bool use_expectation(Diagnosis& diagnosis, const Expectation& expectation) {
    const auto& site = diagnosis.site;

    auto expected = expectation;
    if (diagnosis.failure.kind != ErrorKind::ENUM && site.kind == expected.type) {
        expected.values.clear();
    }
    if (expected.type == JsonKind::STRING && !expected.values.empty()) {
        diagnosis.reason = Reason::EXPECTED_CONSTANT;
        diagnosis.show_actual_kind = site.kind != JsonKind::STRING;
    } else {
        const bool same =
            site.kind == expected.type || (expected.type == JsonKind::INTEGER && site.kind == JsonKind::NUMBER);
        if (expected.type == JsonKind::NONE || same || (site.kind == JsonKind::NULL_VALUE && expected.nullable)) {
            return false;
        }
        diagnosis.reason = Reason::EXPECTED_TYPE;
        diagnosis.show_actual_kind = true;
    }
    diagnosis.expected = std::move(expected);
    return true;
}

}  // namespace

bool needs_location(const ParseFailure& failure, InputFacts input) noexcept {
    return !is_file_error(failure.kind) && !input.empty;
}

SchemaNeed schema_need(const ParseFailure& failure, const Site& site, InputFacts input) {
    switch (classify(failure, site, input)) {
        case Route::MEMBER_NAMES:
            return SchemaNeed::MEMBER_NAMES;
        case Route::EXPECTATION:
            return SchemaNeed::EXPECTATION;
        default:
            return SchemaNeed::NONE;
    }
}

Diagnosis decide(const ParseFailure& failure, const Site& site, InputFacts input, const SchemaFacts& schema) {
    Diagnosis diagnosis;
    diagnosis.site = site;
    diagnosis.failure = failure;
    switch (classify(failure, site, input)) {
        case Route::FILE:
            diagnosis.show_position = false;
            break;
        case Route::EMPTY:
            diagnosis.reason = Reason::EMPTY;
            break;
        case Route::END:
            diagnosis.reason = Reason::END;
            break;
        case Route::MISSING_VALUE:
            diagnosis.reason = Reason::MISSING_VALUE;
            diagnosis.framing = Framing::INVALID_JSON;
            diagnosis.site.path = site.missing_value.path;
            diagnosis.site.line = site.missing_value.line;
            diagnosis.site.column = site.missing_value.column;
            diagnosis.append_location = !diagnosis.site.path.empty();
            break;
        case Route::UNEXPECTED_TOKEN:
            diagnosis.reason = Reason::UNEXPECTED_TOKEN;
            diagnosis.framing = Framing::INVALID_JSON;
            diagnosis.append_location = !diagnosis.site.path.empty();
            break;
        case Route::CONTAINER_BOUNDARY:
            diagnosis.reason = site.after_value.kind == JsonKind::ARRAY ? Reason::ARRAY_SEPARATOR_OR_CLOSE
                                                                        : Reason::OBJECT_SEPARATOR_OR_CLOSE;
            diagnosis.framing = Framing::INVALID_JSON;
            diagnosis.site.path = site.after_value.path;
            diagnosis.site.line = site.after_value.line;
            diagnosis.site.column = site.after_value.column;
            diagnosis.append_location = !diagnosis.site.path.empty();
            break;
        case Route::FALLBACK:
            use_code_fallback(diagnosis);
            break;
        case Route::MEMBER_NAMES:
            diagnosis.reason = Reason::UNKNOWN_KEY;
            diagnosis.candidates = schema.member_names;
            diagnosis.suggestion = closest_key(site.key, schema.member_names);
            break;
        case Route::EXPECTATION:
            if (!use_expectation(diagnosis, schema.expected)) {
                use_code_fallback(diagnosis);
            }
            break;
    }
    return diagnosis;
}

}  // namespace Config::Diagnostic
