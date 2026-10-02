#ifndef YADDNSC_CONFIG_DIAGNOSTIC_DECISION_H
#define YADDNSC_CONFIG_DIAGNOSTIC_DECISION_H

#include <string>
#include <vector>

#include "infrastructure/config/diagnostics/types.h"

namespace Config::Diagnostic {

struct SchemaFacts {
    Expectation expected;
    std::vector<std::string> member_names;
};

struct InputFacts {
    bool empty{false};
    bool at_end{false};
};

enum class Reason {
    CODE,
    EMPTY,
    END,
    MISSING_COLON,
    QUOTED_KEY,
    MALFORMED_STRING,
    MALFORMED_SCALAR,
    UNKNOWN_KEY,
    EXPECTED_TYPE,
    EXPECTED_CONSTANT,
    ARRAY_SEPARATOR_OR_CLOSE,
    OBJECT_SEPARATOR_OR_CLOSE
};
enum class Framing { NONE, INVALID_JSON, READ_FAILURE };

struct Diagnosis {
    Site site;
    ParseFailure failure;
    Reason reason{Reason::CODE};
    Framing framing{Framing::NONE};
    bool show_position{true};
    bool append_location{false};
    bool show_actual_kind{false};
    Expectation expected;
    std::vector<std::string> candidates;
    std::string suggestion;
};

enum class SchemaNeed { NONE, EXPECTATION, MEMBER_NAMES };

/// Fact collection requirements share the final decision's classification policy.
/// File errors and empty input do not require scanning; a default Site suffices.
[[nodiscard]] bool needs_location(const ParseFailure& failure, InputFacts input) noexcept;
[[nodiscard]] SchemaNeed schema_need(const ParseFailure& failure, const Site& site, InputFacts input);

/// Central priority policy: file errors, empty input, malformed tokens, EOF/syntax,
/// schema explanations, then code fallback. All arguments/results are value facts.
[[nodiscard]] Diagnosis decide(const ParseFailure& failure, const Site& site, InputFacts input,
                               const SchemaFacts& schema);

}  // namespace Config::Diagnostic

#endif  // YADDNSC_CONFIG_DIAGNOSTIC_DECISION_H
