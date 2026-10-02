#include "renderer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <yaddnsc/util/format.hpp>

#include "support/fmt.hpp"

namespace Config::Diagnostic {
namespace {
/// Display paths only here; other components use exact key/index components.
[[nodiscard]] std::string render_path(const Path& path) {
    std::string result;
    for (const auto& component : path) {
        if (component.is_index) {
            result += fmt::format("[{}]", component.index);
        } else {
            if (!result.empty()) {
                result += '.';
            }
            result += component.key;
        }
    }
    return result;
}

[[nodiscard]] std::string quote_list(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += fmt::format("\"{}\"", value);
    }
    return out;
}

[[nodiscard]] std::string prose_kind(JsonKind kind) {
    if (kind == JsonKind::STRING) {
        return "a string";
    }
    if (kind == JsonKind::OBJECT) {
        return "an object";
    }
    if (kind == JsonKind::ARRAY) {
        return "an array";
    }
    if (kind == JsonKind::NUMBER) {
        return "a number";
    }
    if (kind == JsonKind::INTEGER) {
        return "a whole number";
    }
    if (kind == JsonKind::BOOLEAN) {
        return "true or false";
    }
    if (kind == JsonKind::NULL_VALUE) {
        return "null";
    }
    return "an unexpected character";
}

/// Wording for a code that the schema cannot explain.
[[nodiscard]] std::string explain_code(const ParseFailure& failure) {
    switch (failure.kind) {
        case ErrorKind::EXPECTED_BRACE:
            return "expected '{' to open an object";
        case ErrorKind::EXPECTED_BRACKET:
            return "expected '[' to open an array";
        case ErrorKind::EXPECTED_QUOTE:
            return "expected a quoted string";
        case ErrorKind::EXPECTED_COMMA:
            return "expected ',' or '}'";
        case ErrorKind::EXPECTED_COLON:
            return "expected ':' after the key";
        case ErrorKind::UNEXPECTED_END:
            return "the file ends in the middle of a value";
        case ErrorKind::SYNTAX:
            return "the text is not valid JSON";
        case ErrorKind::NUMBER:
            return "malformed number";
        case ErrorKind::INVALID_ESCAPE:
            return "invalid escape sequence in a string";
        case ErrorKind::INCOMPLETE_UNICODE:
            return "incomplete \\u escape sequence";
        case ErrorKind::INVALID_UNICODE:
            return "invalid \\u escape sequence";
        case ErrorKind::DEPTH:
            return "the JSON is nested too deeply";
        case ErrorKind::EMPTY:
            return "the file is empty";
        case ErrorKind::FILE_OPEN:
            return "the file could not be opened";
        case ErrorKind::FILE_CLOSE:
            return "the file could not be closed";
        case ErrorKind::FILE_INCLUDE:
            return "an included file could not be read";
        case ErrorKind::FILE_EXTENSION:
            return "the file extension is not supported";
        case ErrorKind::MISSING_KEY:
            return "a required key is missing";
        case ErrorKind::INVALID_NULL:
            return "null is not accepted here";
        case ErrorKind::CONSTRAINT:
            return "a value constraint was violated";
        case ErrorKind::UNKNOWN_KEY:
            return "unknown key";
        case ErrorKind::ENUM:
            return "value is not one of the accepted constants";
        default:
            break;
    }
    // Unknown code: read the name out, e.g. "invalid_flag_input" → "invalid flag input".
    std::string name = failure.identifier;
    std::ranges::replace(name, '_', ' ');
    return name;
}

[[nodiscard]] std::string location_label(const Path& path) {
    const auto text = render_path(path);
    return text.empty() ? std::string{} : fmt::format("\"{}\"", text);
}

[[nodiscard]] std::string render_reason(const Diagnosis& diagnosis) {
    const auto& site = diagnosis.site;
    switch (diagnosis.reason) {
        case Reason::EMPTY:
            return "the file is empty";
        case Reason::END:
            return "the file ends here; a key or a value is missing";
        case Reason::MISSING_COLON:
            return fmt::format("expected ':' after the key \"{}\"", site.key);
        case Reason::QUOTED_KEY:
            return "expected a quoted key";
        case Reason::MALFORMED_STRING:
            return "malformed or unterminated string";
        case Reason::MALFORMED_SCALAR:
            return "malformed scalar value";
        case Reason::UNKNOWN_KEY: {
            std::string reason = site.key.empty() ? "unknown key" : fmt::format("unknown key \"{}\"", site.key);
            if (!site.parent_path.empty()) {
                reason += fmt::format(" in {}", location_label(site.parent_path));
            }
            if (!diagnosis.suggestion.empty()) {
                reason += fmt::format(" — did you mean \"{}\"?", diagnosis.suggestion);
            } else if (!diagnosis.candidates.empty()) {
                reason += fmt::format(" — keys accepted here: {}", quote_list(diagnosis.candidates));
            }
            return reason;
        }
        case Reason::EXPECTED_TYPE: {
            const auto wanted = diagnosis.expected.type == JsonKind::BOOLEAN ? "a boolean (true or false)"
                                                                             : prose_kind(diagnosis.expected.type);
            return fmt::format("{} expects {}, got {}", location_label(site.path), wanted, prose_kind(site.kind));
        }
        case Reason::EXPECTED_CONSTANT: {
            auto reason =
                fmt::format("{} expects one of {}", location_label(site.path), quote_list(diagnosis.expected.values));
            if (diagnosis.show_actual_kind) {
                reason += fmt::format(" (got {})", prose_kind(site.kind));
            }
            return reason;
        }
        case Reason::ARRAY_SEPARATOR_OR_CLOSE:
            return "expected ',' or ']' after an array element";
        case Reason::OBJECT_SEPARATOR_OR_CLOSE:
            return "expected ',' or '}' after an object member";
        case Reason::CODE:
            return explain_code(diagnosis.failure);
    }
    return {};
}

}  // namespace

std::string render(std::string_view config_path, const Diagnosis& diagnosis) {
    auto reason = render_reason(diagnosis);
    switch (diagnosis.framing) {
        case Framing::NONE:
            break;
        case Framing::INVALID_JSON:
            reason.insert(0, "invalid JSON — ");
            break;
        case Framing::READ_FAILURE:
            reason.insert(0, "could not read configuration — ");
            break;
    }
    if (diagnosis.append_location) {
        reason += fmt::format(" at {}", location_label(diagnosis.site.path));
    }
    if (!diagnosis.show_position) {
        return fmt::format("config file \"{}\": {}", config_path, reason);
    }
    return fmt::format("config file \"{}\" (line {}, column {}): {}", config_path, diagnosis.site.line,
                       diagnosis.site.column, reason);
}

}  // namespace Config::Diagnostic
