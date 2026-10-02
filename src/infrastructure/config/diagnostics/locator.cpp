#include "locator.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

namespace Config::Diagnostic {
namespace {
enum class ScanState { KEY, COLON, VALUE, SEPARATOR };

/// Paths live in one component stack, not in a copied string per container.
struct Frame {
    char container{};
    std::size_t path_size{};
    std::size_t index{};
    ScanState state{ScanState::VALUE};
    bool value_complete{false};
    bool separator_seen{false};
    std::string key;
    std::size_t key_open{};
};

constexpr std::size_t NO_OFFSET = static_cast<std::size_t>(-1);

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// JSON kind named by a single character.
[[nodiscard]] JsonKind classify(char c) {
    switch (c) {
        case '"':
            return JsonKind::STRING;
        case '{':
            return JsonKind::OBJECT;
        case '[':
            return JsonKind::ARRAY;
        case 't':
        case 'f':
            return JsonKind::BOOLEAN;
        case 'n':
            return JsonKind::NULL_VALUE;
        default:
            if ((c >= '0' && c <= '9') || c == '-') {
                return JsonKind::NUMBER;
            }
            return JsonKind::UNEXPECTED;
    }
}

/// Recognize only a local scalar token, not the validity of the document.
/// A prefix such as `tru` or `2x` is not evidence of a JSON type mismatch.
[[nodiscard]] JsonKind value_kind(std::string_view buffer, std::size_t start) {
    const auto kind = classify(buffer[start]);
    const auto boundary = [buffer](std::size_t at) {
        return at == buffer.size() || is_space(buffer[at]) || buffer[at] == ',' || buffer[at] == ']' ||
               buffer[at] == '}';
    };
    if (kind == JsonKind::BOOLEAN || kind == JsonKind::NULL_VALUE) {
        const std::string_view literal = buffer[start] == 't' ? "true" : buffer[start] == 'f' ? "false" : "null";
        return buffer.substr(start).starts_with(literal) && boundary(start + literal.size()) ? kind
                                                                                             : JsonKind::UNEXPECTED;
    }
    if (kind != JsonKind::NUMBER) {
        return kind;
    }
    std::size_t i = start;
    if (buffer[i] == '-') {
        ++i;
    }
    const auto digit = [buffer](std::size_t at) {
        return at < buffer.size() && buffer[at] >= '0' && buffer[at] <= '9';
    };
    if (!digit(i)) {
        return JsonKind::UNEXPECTED;
    }
    if (buffer[i] == '0') {
        ++i;
    } else {
        while (digit(i)) {
            ++i;
        }
    }
    if (i < buffer.size() && buffer[i] == '.') {
        if (!digit(++i)) {
            return JsonKind::UNEXPECTED;
        }
        while (digit(i)) {
            ++i;
        }
    }
    if (i < buffer.size() && (buffer[i] == 'e' || buffer[i] == 'E')) {
        ++i;
        if (i < buffer.size() && (buffer[i] == '+' || buffer[i] == '-')) {
            ++i;
        }
        if (!digit(i)) {
            return JsonKind::UNEXPECTED;
        }
        while (digit(i)) {
            ++i;
        }
    }
    return boundary(i) ? kind : JsonKind::UNEXPECTED;
}

/// The first token at or after @p offset: where it starts and what it is.
struct NextToken {
    std::size_t offset{NO_OFFSET};
    JsonKind kind{JsonKind::NONE};
};

[[nodiscard]] NextToken next_token(std::string_view buffer, std::size_t offset) {
    for (std::size_t i = std::min(offset, buffer.size()); i < buffer.size(); ++i) {
        if (is_space(buffer[i])) {
            continue;
        }
        return {.offset = i, .kind = value_kind(buffer, i)};
    }
    return {};
}

/// 1-based line and column of @p offset.
[[nodiscard]] std::pair<std::size_t, std::size_t> line_column(std::string_view buffer, std::size_t offset) {
    std::size_t line{1};
    std::size_t column{1};
    for (std::size_t i = 0; i < offset && i < buffer.size(); ++i) {
        if (buffer[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

/// Whether the member name ending just before @p end is followed by ':'.
[[nodiscard]] bool colon_follows(std::string_view buffer, std::size_t end) {
    for (std::size_t i = end + 1; i < buffer.size(); ++i) {
        if (is_space(buffer[i])) {
            continue;
        }
        return buffer[i] == ':';
    }
    return false;
}

/// Scan a string locally without storing its value. Keys alone are decoded.
struct StringToken {
    std::size_t end{};  ///< closing quote, or buffer.size() for a truncated string
    bool malformed{false};
};

[[nodiscard]] StringToken scan_string(std::string_view buffer, std::size_t start, std::size_t limit) {
    bool malformed{false};
    for (std::size_t i = start + 1; i < buffer.size() && i <= limit; ++i) {
        const char c = buffer[i];
        if (c == '"') {
            return {.end = i, .malformed = malformed};
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            malformed = true;
        }
        if (c != '\\') {
            continue;
        }
        if (++i >= buffer.size()) {
            return {.end = buffer.size(), .malformed = true};
        }
        const char escaped = buffer[i];
        if (escaped == 'u') {
            for (std::size_t digit = 0; digit < 4; ++digit) {
                if (++i >= buffer.size()) {
                    return {.end = buffer.size(), .malformed = true};
                }
                const char hex = buffer[i];
                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F'))) {
                    malformed = true;
                }
            }
        } else if (std::string_view{R"("\/bfnrt)"}.find(escaped) == std::string_view::npos) {
            malformed = true;
        }
    }
    return {.end = std::min(buffer.size(), limit + (limit < buffer.size() ? 1U : 0U)),
            .malformed = malformed || limit >= buffer.size()};
}

/// Rebuild only the prefix containing the failure. Each value, including every
/// scalar array element, enters the same state exactly once.
}  // namespace

Site locate(std::string_view buffer, std::size_t offset, ScanPolicy policy) {
    Site site;
    std::vector<Frame> stack;
    std::vector<PathComponent> path;
    std::size_t value_open{NO_OFFSET};
    std::size_t report = std::min(offset, buffer.size());
    const std::size_t limit = report;

    const auto begin_value = [&](std::size_t at) {
        if (!stack.empty()) {
            auto& parent = stack.back();
            path.resize(parent.path_size);
            if (parent.container == '[') {
                path.push_back(PathComponent{.key = {}, .index = parent.index++, .is_index = true});
            } else {
                path.push_back({.key = parent.key});
            }
            parent.state = ScanState::SEPARATOR;
            parent.value_complete = false;
        }
        value_open = at;
    };
    // The first unconditional syntax damage wins: a separator or a closing
    // bracket/brace where a value is required. Recorded whether the scan stops
    // at the offending token or the reader's offset lies beyond it.
    const auto note_missing_value = [&](std::size_t at) {
        if (site.missing_value.container != JsonKind::NONE || stack.empty()) {
            return;
        }
        const auto& frame = stack.back();
        site.missing_value.container = frame.container == '[' ? JsonKind::ARRAY : JsonKind::OBJECT;
        site.missing_value.path = path;
        site.missing_value.path.resize(frame.path_size);
        if (frame.container == '{' && !frame.key.empty()) {
            site.missing_value.path.push_back({.key = frame.key});
        }
        site.missing_value.found = buffer[at];
        const auto [line, column] = line_column(buffer, at);
        site.missing_value.line = line;
        site.missing_value.column = column;
    };
    const auto capture = [&] {
        if (!stack.empty() && stack.back().state == ScanState::SEPARATOR && stack.back().value_complete) {
            const auto& frame = stack.back();
            const auto token = next_token(buffer, limit);
            const auto boundary = token.offset == NO_OFFSET ? buffer.size() : token.offset;
            const char close = frame.container == '[' ? ']' : '}';
            if (boundary == buffer.size() || (buffer[boundary] != ',' && buffer[boundary] != close)) {
                site.after_value.kind = frame.container == '[' ? JsonKind::ARRAY : JsonKind::OBJECT;
                site.after_value.path = path;
                site.after_value.path.resize(frame.path_size);
                const auto [line, column] = line_column(buffer, boundary);
                site.after_value.line = line;
                site.after_value.column = column;
            }
        }
        if (!stack.empty() && (stack.back().state == ScanState::COLON ||
                               (stack.back().container == '{' && stack.back().state == ScanState::VALUE))) {
            const auto& frame = stack.back();
            site.path = path;
            site.path.resize(frame.path_size);
            site.parent_path = site.path;
            site.key = frame.key;
            site.path.push_back({.key = frame.key});
            site.at_key = frame.state == ScanState::COLON;
            if (site.at_key) {
                report = frame.key_open;
                site.key_without_colon = true;
            } else {
                const auto token = next_token(buffer, limit);
                report = token.offset == NO_OFFSET ? limit : token.offset;
                site.kind = token.kind;
                // A separator or close right after ':' is a missing member value.
                if (token.offset != NO_OFFSET &&
                    (buffer[token.offset] == ',' || buffer[token.offset] == '}' || buffer[token.offset] == ']')) {
                    note_missing_value(token.offset);
                }
            }
        } else {
            site.path = path;
            site.parent_path = path;
            site.at_key = !stack.empty() && stack.back().state == ScanState::KEY;
            if (value_open != NO_OFFSET) {
                report = value_open;
                site.kind = value_kind(buffer, value_open);
                site.malformed_scalar =
                    site.kind == JsonKind::UNEXPECTED && classify(buffer[value_open]) != JsonKind::UNEXPECTED;
            }
        }
    };

    std::size_t i = 0;
    while (i < buffer.size()) {
        if (is_space(buffer[i])) {
            if (i >= limit) {
                break;
            }
            ++i;
            continue;
        }
        const char c = buffer[i];
        const bool wants_key = !stack.empty() && stack.back().container == '{' && stack.back().state == ScanState::KEY;
        const bool wants_value = stack.empty() ? value_open == NO_OFFSET : stack.back().state == ScanState::VALUE;

        if (c == '"' && wants_key) {
            // Unknown-key offsets may be inside the key or just after its quote.
            // Reading its end and following colon is the only required lookahead.
            const auto token = scan_string(buffer, i, buffer.size());
            auto& frame = stack.back();
            frame.key_open = i;
            const auto text = buffer.substr(i, token.end - i + (token.end < buffer.size() ? 1U : 0U));
            if (token.malformed || glz::read_json(frame.key, text)) {
                frame.key.assign(buffer.substr(i + 1, token.end - i - 1));
            }
            frame.state = ScanState::COLON;
            value_open = NO_OFFSET;
            if (limit <= token.end || (policy.key_offset_after_quote && limit <= token.end + 1)) {
                capture();
                site.key_without_colon = token.end >= buffer.size() || !colon_follows(buffer, token.end);
                site.malformed_string = token.malformed;
                const auto [line, column] = line_column(buffer, report);
                site.line = line;
                site.column = column;
                return site;
            }
            i = token.end < buffer.size() ? token.end + 1 : token.end;
            continue;
        }

        // A stray separator starts no value; the separator branch below consumes it.
        if (wants_value && c != ']' && c != '}' && c != ',') {
            begin_value(i);
            if (i >= limit && !(c == '"' && policy.inspect_complete_string)) {
                break;
            }
            if (c == '{' || c == '[') {
                stack.push_back(Frame{.container = c,
                                      .path_size = path.size(),
                                      .index = 0,
                                      .state = c == '{' ? ScanState::KEY : ScanState::VALUE,
                                      .key = {},
                                      .key_open = 0});
                value_open = NO_OFFSET;
                ++i;
                continue;
            }
            if (c == '"') {
                // Enum readers may reject a truncated/escaped string as an enum;
                // inspect only that token, never the rest of the document.
                const auto token = scan_string(buffer, i, policy.inspect_complete_string ? buffer.size() : limit);
                site.malformed_string = token.malformed;
                if (limit <= token.end) {
                    break;
                }
                if (!stack.empty()) {
                    stack.back().value_complete = !token.malformed;
                }
                i = token.end + 1;
                continue;
            }
            const auto start = i;
            ++i;
            while (i < buffer.size() && i < limit && !is_space(buffer[i]) && buffer[i] != ',' && buffer[i] != ']' &&
                   buffer[i] != '}') {
                ++i;
            }
            if (!stack.empty()) {
                const bool at_boundary = i == buffer.size() || is_space(buffer[i]) || buffer[i] == ',' ||
                                         buffer[i] == ']' || buffer[i] == '}';
                stack.back().value_complete = at_boundary && value_kind(buffer, start) != JsonKind::UNEXPECTED;
            }
            continue;
        }
        if (i >= limit) {
            break;
        }
        if (c == ':' && !stack.empty() && stack.back().state == ScanState::COLON) {
            stack.back().state = ScanState::VALUE;
        } else if (c == ',' && !stack.empty()) {
            // A comma while awaiting a value (after ':' or a separator) is
            // stray, not an element boundary; some readers report past it.
            if (stack.back().state == ScanState::VALUE) {
                note_missing_value(i);
            }
            auto& frame = stack.back();
            path.resize(frame.path_size);
            frame.state = frame.container == '{' ? ScanState::KEY : ScanState::VALUE;
            frame.key.clear();
            frame.value_complete = false;
            frame.separator_seen = true;
            value_open = NO_OFFSET;
        } else if ((c == '}' || c == ']') && !stack.empty()) {
            const auto& frame = stack.back();
            // A close while awaiting a value is premature; only '[' + ']' with
            // nothing consumed yet is the legal empty array.
            if (frame.state == ScanState::VALUE &&
                !(frame.container == '[' && c == ']' && frame.index == 0 && !frame.separator_seen)) {
                note_missing_value(i);
            }
            const bool complete = c == (frame.container == '[' ? ']' : '}') &&
                                  ((frame.state == ScanState::SEPARATOR && frame.value_complete) ||
                                   (frame.container == '[' && frame.state == ScanState::VALUE && frame.index == 0 &&
                                    !frame.separator_seen) ||
                                   (frame.container == '{' && frame.state == ScanState::KEY && frame.key.empty()));
            stack.pop_back();
            if (!stack.empty()) {
                stack.back().value_complete = complete;
            }
            // Some typed readers report after closing the enclosing containers.
            // Retain the last leaf until the next key/value or separator starts.
        }
        ++i;
    }
    // Awaiting an array element when the scan stops: a separator here, or a
    // close after a consumed separator, is unconditional syntax damage (a
    // leading, double, or trailing comma). An offset on whitespace before a
    // real element still names that element; a cleanly closed empty array is fine.
    if (!stack.empty() && stack.back().container == '[' && stack.back().state == ScanState::VALUE) {
        const auto token = next_token(buffer, i);
        if (token.offset != NO_OFFSET) {
            const char found = buffer[token.offset];
            if (found == ',' || found == '}' || (found == ']' && stack.back().separator_seen)) {
                note_missing_value(token.offset);
            } else if (found != ']') {
                begin_value(token.offset);
            }
        }
    }
    capture();
    const auto [line, column] = line_column(buffer, std::min(report, buffer.size()));
    site.line = line;
    site.column = column;
    return site;
}

}  // namespace Config::Diagnostic
