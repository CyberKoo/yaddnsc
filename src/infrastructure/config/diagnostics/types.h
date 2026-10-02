#ifndef YADDNSC_CONFIG_DIAGNOSTIC_TYPES_H
#define YADDNSC_CONFIG_DIAGNOSTIC_TYPES_H

#include <cstddef>
#include <string>
#include <vector>

namespace Config::Diagnostic {

// Internal diagnostic vocabulary. No configuration values are retained.
enum class JsonKind { NONE, STRING, OBJECT, ARRAY, BOOLEAN, NULL_VALUE, NUMBER, INTEGER, UNEXPECTED };

struct PathComponent {
    std::string key;
    std::size_t index{};
    bool is_index{false};
};

using Path = std::vector<PathComponent>;

/// Present only when a complete value precedes an invalid separator or EOF.
/// This is lexical completion, not schema acceptance of the value.
struct ContainerBoundary {
    JsonKind kind{JsonKind::NONE};
    Path path;
    std::size_t line{1};
    std::size_t column{1};
};

struct Site {
    std::size_t line{1};
    std::size_t column{1};
    Path parent_path;
    std::string key;
    JsonKind kind{JsonKind::NONE};
    bool at_key{false};
    bool key_without_colon{false};
    Path path;
    bool malformed_string{false};
    bool malformed_scalar{false};
    ContainerBoundary after_value{};
};

enum class ErrorKind {
    EXPECTED_BRACE,
    EXPECTED_BRACKET,
    EXPECTED_QUOTE,
    EXPECTED_COMMA,
    EXPECTED_COLON,
    UNEXPECTED_END,
    SYNTAX,
    NUMBER,
    INVALID_ESCAPE,
    INCOMPLETE_UNICODE,
    INVALID_UNICODE,
    DEPTH,
    EMPTY,
    FILE_OPEN,
    FILE_CLOSE,
    FILE_INCLUDE,
    FILE_EXTENSION,
    MISSING_KEY,
    INVALID_NULL,
    CONSTRAINT,
    UNKNOWN_KEY,
    ENUM,
    OTHER
};

struct ParseFailure {
    ErrorKind kind{ErrorKind::OTHER};
    // Library identifier only, not prose or input content, for unmapped codes.
    std::string identifier;
};

struct Expectation {
    JsonKind type{JsonKind::NONE};
    std::vector<std::string> values;  // Schema constants, never input values.
    bool nullable{false};
};

}  // namespace Config::Diagnostic

#endif  // YADDNSC_CONFIG_DIAGNOSTIC_TYPES_H
